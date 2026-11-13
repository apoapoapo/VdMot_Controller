#include <string.h>
#include <unity.h>

#include "vdm/codec.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t buf[VDM_PAYLOAD_MAX];
static size_t n;
static uint8_t bad;

/* ---------------------------------------------------------------------- */

static void test_response_ok_with_body(void)
{
    const uint8_t body[] = {0xAA, 0xBB};
    TEST_ASSERT_TRUE(vdm_enc_response(VDM_OK, 0, body, 2, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(3, n);

    uint8_t st, det;
    const uint8_t *b;
    size_t bl;
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_response(buf, n, &st, &det, &b, &bl));
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, st);
    TEST_ASSERT_EQUAL_size_t(2, bl);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(body, b, 2);
}

static void test_response_errors(void)
{
    uint8_t st, det;
    const uint8_t *b;
    size_t bl;

    TEST_ASSERT_TRUE(vdm_enc_response(VDM_ERR_RANGE, 4, NULL, 0, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_response(buf, n, &st, &det, &b, &bl));
    TEST_ASSERT_EQUAL_UINT8(VDM_ERR_RANGE, st);
    TEST_ASSERT_EQUAL_UINT8(4, det);
    TEST_ASSERT_EQUAL_size_t(0, bl);

    /* body is ignored for error responses */
    const uint8_t body[] = {1};
    TEST_ASSERT_TRUE(vdm_enc_response(VDM_ERR_BUSY, 0, body, 1, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(1, n);

    TEST_ASSERT_EQUAL(VDM_ERR_BAD_LEN, vdm_dec_response(buf, 0, &st, &det, &b, &bl));
    const uint8_t range_no_detail[] = {VDM_ERR_RANGE};
    TEST_ASSERT_EQUAL(VDM_ERR_BAD_LEN, vdm_dec_response(range_no_detail, 1, &st, &det, &b, &bl));
    const uint8_t busy_extra[] = {VDM_ERR_BUSY, 0};
    TEST_ASSERT_EQUAL(VDM_ERR_BAD_LEN, vdm_dec_response(busy_extra, 2, &st, &det, &b, &bl));
}

static void test_mask_channel_offset_u16(void)
{
    uint16_t m;
    TEST_ASSERT_TRUE(vdm_enc_mask(0x0805, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_HEX8(0x05, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x08, buf[1]);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_mask(buf, n, &m));
    TEST_ASSERT_EQUAL_HEX16(0x0805, m);
    TEST_ASSERT_FALSE(vdm_enc_mask(0x1000, buf, sizeof(buf), &n));
    const uint8_t hi[] = {0x00, 0x10};
    TEST_ASSERT_EQUAL(VDM_ERR_CHANNEL, vdm_dec_mask(hi, 2, &m));
    TEST_ASSERT_EQUAL(VDM_ERR_BAD_LEN, vdm_dec_mask(hi, 1, &m));

    uint8_t ch;
    TEST_ASSERT_TRUE(vdm_enc_channel(11, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_channel(buf, n, &ch));
    TEST_ASSERT_EQUAL_UINT8(11, ch);
    TEST_ASSERT_FALSE(vdm_enc_channel(12, buf, sizeof(buf), &n));
    const uint8_t c12[] = {12};
    TEST_ASSERT_EQUAL(VDM_ERR_CHANNEL, vdm_dec_channel(c12, 1, &ch));

    uint8_t off;
    TEST_ASSERT_TRUE(vdm_enc_offset(24, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_offset(buf, n, &off));
    TEST_ASSERT_EQUAL_UINT8(24, off);

    uint16_t v;
    TEST_ASSERT_TRUE(vdm_enc_u16(0xBEEF, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_u16(buf, n, &v));
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, v);
}

static void test_reset_magic(void)
{
    TEST_ASSERT_TRUE(vdm_enc_reset(buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(VDM_RESET_LEN, n);
    const uint8_t expect[] = {0x31, 0x54, 0x53, 0x52}; /* "RST1" little-endian */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf, 4);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_reset(buf, n));
    buf[0] ^= 1;
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_reset(buf, n));
}

static void test_hello(void)
{
    vdm_hello_req_t rq = {.esp_proto_version = 1, .flags = VDM_HELLO_FLAG_RESET_SEQ}, rq2;
    TEST_ASSERT_TRUE(vdm_enc_hello_req(&rq, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(VDM_HELLO_REQ_LEN, n);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_hello_req(buf, n, &rq2));
    TEST_ASSERT_EQUAL_MEMORY(&rq, &rq2, sizeof(rq));

    vdm_hello_t h = {
        .proto_version = 1,
        .fw_major = 2,
        .fw_minor = 0,
        .fw_patch = 3,
        .fw_build = 0x01020304,
        .hw_rev = VDM_HW_C2,
        .mcu = VDM_MCU_F411,
        .mcu_uid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
        .channel_count = 12,
        .capabilities = VDM_CAP_DS18B20 | VDM_CAP_DS2438,
        .reset_cause = VDM_RESET_IWDG,
        .boot_count = 4711,
        .config_crc = 0xA55A,
    };
    vdm_hello_t h2;
    TEST_ASSERT_TRUE(vdm_enc_hello(&h, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(VDM_HELLO_LEN, n);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[4]); /* fw_build little-endian */
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[7]);
    memset(&h2, 0, sizeof(h2));
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_hello(buf, n, &h2));
    TEST_ASSERT_EQUAL_UINT32(h.fw_build, h2.fw_build);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(h.mcu_uid, h2.mcu_uid, 12);
    TEST_ASSERT_EQUAL_UINT32(h.capabilities, h2.capabilities);
    TEST_ASSERT_EQUAL_UINT32(h.boot_count, h2.boot_count);
    TEST_ASSERT_EQUAL_HEX16(h.config_crc, h2.config_crc);
    TEST_ASSERT_EQUAL_UINT8(h.reset_cause, h2.reset_cause);
    TEST_ASSERT_EQUAL(VDM_ERR_BAD_LEN, vdm_dec_hello(buf, n - 1, &h2));
}

static void test_system_config_roundtrip_and_defaults(void)
{
    vdm_system_config_t c = vdm_system_config_default, c2;
    TEST_ASSERT_EQUAL_UINT16(VDM_POS_MAX, c.failsafe_pos); /* failsafe = fully open */
    TEST_ASSERT_TRUE(vdm_enc_system_config(&c, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(VDM_SYSTEM_CONFIG_LEN, n);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_system_config(buf, n, &c2, &bad));
    TEST_ASSERT_EQUAL_MEMORY(&c, &c2, sizeof(c));
}

static void expect_sys_range(vdm_system_config_t c, uint8_t field)
{
    uint8_t raw[VDM_SYSTEM_CONFIG_LEN];
    /* encode manually: the encoder refuses invalid configs */
    TEST_ASSERT_FALSE(vdm_enc_system_config(&c, buf, sizeof(buf), &n));
    raw[0] = (uint8_t)c.link_timeout_s;
    raw[1] = (uint8_t)(c.link_timeout_s >> 8);
    raw[2] = (uint8_t)c.failsafe_pos;
    raw[3] = (uint8_t)(c.failsafe_pos >> 8);
    raw[4] = (uint8_t)c.startup_pos;
    raw[5] = (uint8_t)(c.startup_pos >> 8);
    raw[6] = (uint8_t)c.sensor_interval_s;
    raw[7] = (uint8_t)(c.sensor_interval_s >> 8);
    raw[8] = (uint8_t)c.recal_after_moves;
    raw[9] = (uint8_t)(c.recal_after_moves >> 8);
    raw[10] = c.recal_after_days;
    vdm_system_config_t out;
    bad = 0xEE;
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_system_config(raw, sizeof(raw), &out, &bad));
    TEST_ASSERT_EQUAL_UINT8(field, bad);
}

static void test_system_config_ranges(void)
{
    vdm_system_config_t c = vdm_system_config_default;
    c.link_timeout_s = 0; /* 0 = never: valid */
    TEST_ASSERT_TRUE(vdm_enc_system_config(&c, buf, sizeof(buf), &n));
    c.failsafe_pos = VDM_POS_HOLD;
    c.startup_pos = 0;
    c.recal_after_moves = 0;
    c.recal_after_days = 0;
    TEST_ASSERT_TRUE(vdm_enc_system_config(&c, buf, sizeof(buf), &n));

    c = vdm_system_config_default;
    c.link_timeout_s = 9;
    expect_sys_range(c, 0);
    c = vdm_system_config_default;
    c.link_timeout_s = 3601;
    expect_sys_range(c, 0);
    c = vdm_system_config_default;
    c.failsafe_pos = 1001;
    expect_sys_range(c, 1);
    c = vdm_system_config_default;
    c.startup_pos = 0xFFFE;
    expect_sys_range(c, 2);
    c = vdm_system_config_default;
    c.sensor_interval_s = 4;
    expect_sys_range(c, 3);
    c = vdm_system_config_default;
    c.recal_after_moves = 49;
    expect_sys_range(c, 4);
    c = vdm_system_config_default;
    c.recal_after_days = 61;
    expect_sys_range(c, 5);
}

static void test_motor_params(void)
{
    vdm_motor_params_t m = vdm_motor_params_default, m2;
    TEST_ASSERT_TRUE(vdm_enc_motor_params(&m, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(VDM_MOTOR_PARAMS_LEN, n);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_motor_params(buf, n, &m2, &bad));
    TEST_ASSERT_EQUAL_MEMORY(&m, &m2, sizeof(m));

    /* min_stroke_counts is field 7 at bytes 12..13 */
    buf[12] = 50;
    buf[13] = 0;
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_motor_params(buf, n, &m2, &bad));
    TEST_ASSERT_EQUAL_UINT8(7, bad);
    /* endstop_factor_close is field 1 */
    TEST_ASSERT_TRUE(vdm_enc_motor_params(&m, buf, sizeof(buf), &n));
    buf[1] = 10;
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_motor_params(buf, n, &m2, &bad));
    TEST_ASSERT_EQUAL_UINT8(1, bad);

    m.max_calib_retries = 6;
    TEST_ASSERT_FALSE(vdm_enc_motor_params(&m, buf, sizeof(buf), &n));
}

/* Payload bytes of the EVT_VALVE example in the specification, chapter 8 */
static void test_valve_state_spec_example(void)
{
    vdm_valve_state_t s = {.channel = 3,
                           .mode = VDM_MODE_OPENING,
                           .flags = VDM_VFLAG_CALIBRATED,
                           .fault = VDM_FAULT_NONE,
                           .actual = 300,
                           .target = 450,
                           .current_ma10 = 210};
    const uint8_t expect[] = {0x03, 0x04, 0x01, 0x00, 0x2C, 0x01, 0xC2, 0x01, 0xD2, 0x00};
    TEST_ASSERT_TRUE(vdm_enc_valve_state(&s, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(VDM_VALVE_STATE_LEN, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf, sizeof(expect));

    vdm_valve_state_t s2;
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_valve_state(expect, sizeof(expect), &s2, &bad));
    TEST_ASSERT_EQUAL_MEMORY(&s, &s2, sizeof(s));
}

static void test_valve_state_validation(void)
{
    vdm_valve_state_t s = {
        .channel = 0, .mode = VDM_MODE_IDLE, .actual = VDM_POS_UNKNOWN, .target = 0};
    TEST_ASSERT_TRUE(vdm_enc_valve_state(&s, buf, sizeof(buf), &n)); /* unknown ok */
    s.target = VDM_POS_UNKNOWN;
    TEST_ASSERT_FALSE(vdm_enc_valve_state(&s, buf, sizeof(buf), &n)); /* not for target */

    uint8_t raw[] = {0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    vdm_valve_state_t out;
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_valve_state(raw, sizeof(raw), &out, &bad));
    TEST_ASSERT_EQUAL_UINT8(1, bad);
    raw[1] = VDM_MODE_IDLE;
    raw[2] = 0x20; /* reserved flag bit */
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_valve_state(raw, sizeof(raw), &out, &bad));
    TEST_ASSERT_EQUAL_UINT8(2, bad);
    raw[2] = 0;
    raw[0] = 12;
    TEST_ASSERT_EQUAL(VDM_ERR_CHANNEL, vdm_dec_valve_state(raw, sizeof(raw), &out, &bad));
}

static void test_valve_states_list(void)
{
    vdm_valve_states_t in = {.count = VDM_MAX_CHANNELS}, out;
    for (uint8_t i = 0; i < VDM_MAX_CHANNELS; i++) {
        in.valves[i] = (vdm_valve_state_t){.channel = i,
                                           .mode = VDM_MODE_IDLE,
                                           .flags = VDM_VFLAG_CALIBRATED,
                                           .actual = (uint16_t)(i * 80),
                                           .target = (uint16_t)(i * 80),
                                           .current_ma10 = (uint16_t)(200 + i)};
    }
    TEST_ASSERT_TRUE(vdm_enc_valve_states(&in, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(1 + 12 * VDM_VALVE_STATE_LEN, n);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_valve_states(buf, n, &out, &bad));
    TEST_ASSERT_EQUAL_UINT8(12, out.count);
    TEST_ASSERT_EQUAL_MEMORY(in.valves, out.valves, sizeof(in.valves));

    TEST_ASSERT_EQUAL(VDM_ERR_BAD_LEN, vdm_dec_valve_states(buf, n - 1, &out, &bad));
    buf[1 + 5 * VDM_VALVE_STATE_LEN + 1] = 0x7F; /* mode of entry 5 */
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_valve_states(buf, n, &out, &bad));
    TEST_ASSERT_EQUAL_UINT8(5, bad);
}

/* Payload of the SET_TARGETS example in the specification, chapter 8 */
static void test_set_targets(void)
{
    const uint8_t spec[] = {0x01, 0x03, 0xC2, 0x01};
    vdm_set_targets_t t;
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_set_targets(spec, sizeof(spec), &t, &bad));
    TEST_ASSERT_EQUAL_UINT8(1, t.count);
    TEST_ASSERT_EQUAL_UINT8(3, t.targets[0].channel);
    TEST_ASSERT_EQUAL_UINT16(450, t.targets[0].target);
    TEST_ASSERT_TRUE(vdm_enc_set_targets(&t, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(spec, buf, sizeof(spec));

    const uint8_t bad_target[] = {0x02, 0x00, 0x00, 0x00, 0x01, 0xE9, 0x03}; /* 1001 */
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_set_targets(bad_target, 7, &t, &bad));
    TEST_ASSERT_EQUAL_UINT8(1, bad);
    const uint8_t bad_ch[] = {0x01, 0x0C, 0x00, 0x00};
    TEST_ASSERT_EQUAL(VDM_ERR_CHANNEL, vdm_dec_set_targets(bad_ch, 4, &t, &bad));
    TEST_ASSERT_EQUAL_UINT8(0, bad);
    const uint8_t zero_count[] = {0x00};
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_set_targets(zero_count, 1, &t, &bad));
    TEST_ASSERT_EQUAL(VDM_ERR_BAD_LEN, vdm_dec_set_targets(spec, 3, &t, &bad));
}

static void test_valve_detail(void)
{
    vdm_valve_detail_t d = {
        .state = {.channel = 7,
                  .mode = VDM_MODE_FAULT,
                  .fault = VDM_FAULT_BLOCKED,
                  .actual = 120,
                  .target = 500},
        .open_counts = 13754,
        .close_counts = 10907,
        .deadzone_counts = -2847,
        .calib_age_h = 0xFFFFFFFFu,
        .moves_since_calib = 12,
        .moves_total = 99999,
        .mean_current_open_ma10 = 215,
        .mean_current_close_ma10 = 198,
        .peak_current_ma10 = 812,
        .last_move_counts = -1200,
        .last_move_ms = 8300,
        .calib_retries = 2,
        .last_fault = VDM_FAULT_TIMEOUT,
    };
    vdm_valve_detail_t d2;
    TEST_ASSERT_TRUE(vdm_enc_valve_detail(&d, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(VDM_VALVE_DETAIL_LEN, n);
    memset(&d2, 0, sizeof(d2));
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_valve_detail(buf, n, &d2, &bad));
    TEST_ASSERT_EQUAL_INT32(-2847, d2.deadzone_counts);
    TEST_ASSERT_EQUAL_INT32(-1200, d2.last_move_counts);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, d2.calib_age_h);
    TEST_ASSERT_EQUAL_UINT16(812, d2.peak_current_ma10);
    TEST_ASSERT_EQUAL_UINT8(VDM_FAULT_TIMEOUT, d2.last_fault);
    TEST_ASSERT_EQUAL_MEMORY(&d.state, &d2.state, sizeof(d.state));

    buf[0] = 6; /* channel mismatch with embedded state */
    TEST_ASSERT_EQUAL(VDM_ERR_CHANNEL, vdm_dec_valve_detail(buf, n, &d2, &bad));
}

static void test_sensors(void)
{
    vdm_sensors_t s = {.total = 14, .offset = 12, .count = 2};
    const uint8_t rom[8] = {0x28, 0x84, 0x37, 0x94, 0x97, 0xFF, 0x03, 0x23};
    memcpy(s.sensors[0].rom, rom, 8);
    s.sensors[0].kind = VDM_SENSOR_TEMP_DS18B20;
    s.sensors[0].status = VDM_SSTAT_OK;
    s.sensors[0].age_s = 3;
    s.sensors[0].value = -12500; /* -12.5 °C */
    memcpy(s.sensors[1].rom, rom, 8);
    s.sensors[1].rom[0] = 0x26;
    s.sensors[1].kind = VDM_SENSOR_VAD_DS2438;
    s.sensors[1].status = VDM_SSTAT_NOT_MEASURED;
    s.sensors[1].age_s = 0xFFFF;
    s.sensors[1].value = 4980;

    vdm_sensors_t s2;
    TEST_ASSERT_TRUE(vdm_enc_sensors(&s, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(3 + 2 * VDM_SENSOR_VALUE_LEN, n);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_sensors(buf, n, &s2, &bad));
    TEST_ASSERT_EQUAL_UINT8(14, s2.total);
    TEST_ASSERT_EQUAL_UINT8(12, s2.offset);
    TEST_ASSERT_EQUAL_INT32(-12500, s2.sensors[0].value);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rom, s2.sensors[0].rom, 8);
    TEST_ASSERT_EQUAL_UINT8(VDM_SENSOR_VAD_DS2438, s2.sensors[1].kind);

    buf[3 + VDM_SENSOR_VALUE_LEN + 8] = 9; /* kind of entry 1 */
    TEST_ASSERT_EQUAL(VDM_ERR_RANGE, vdm_dec_sensors(buf, n, &s2, &bad));
    TEST_ASSERT_EQUAL_UINT8(1, bad);

    /* a full message fits into one frame */
    s.count = VDM_SENSORS_PER_MSG;
    for (unsigned i = 2; i < VDM_SENSORS_PER_MSG; i++)
        s.sensors[i] = s.sensors[0];
    TEST_ASSERT_TRUE(vdm_enc_sensors(&s, buf, sizeof(buf), &n));
    TEST_ASSERT_TRUE(n + 1 <= VDM_PAYLOAD_MAX); /* +1: response status byte */
    s.count = VDM_SENSORS_PER_MSG + 1;
    TEST_ASSERT_FALSE(vdm_enc_sensors(&s, buf, sizeof(buf), &n));
}

static void test_diag(void)
{
    vdm_diag_t d = {.uptime_s = 86400,
                    .rx_frames_ok = 123456,
                    .rx_crc_err = 1,
                    .rx_malformed = 2,
                    .rx_overflow = 3,
                    .rx_bad_version = 4,
                    .rx_duplicate = 5,
                    .uart_hw_err = 6,
                    .ow_crc_err = 7,
                    .ow_bus_err = 8,
                    .eeprom_writes = 900,
                    .eeprom_err = 9,
                    .loop_max_us = 1500,
                    .mcu_temp_c10 = -105};
    vdm_diag_t d2;
    TEST_ASSERT_TRUE(vdm_enc_diag(&d, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(VDM_DIAG_LEN, n);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_diag(buf, n, &d2));
    TEST_ASSERT_EQUAL_MEMORY(&d, &d2, sizeof(d));
}

static void test_events(void)
{
    vdm_evt_calibration_t c = {.channel = 2,
                               .result = VDM_FAULT_NONE,
                               .open_counts = 13754,
                               .close_counts = 10907,
                               .retries = 1},
                          c2;
    TEST_ASSERT_TRUE(vdm_enc_evt_calibration(&c, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(VDM_EVT_CALIBRATION_LEN, n);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_evt_calibration(buf, n, &c2, &bad));
    TEST_ASSERT_EQUAL_MEMORY(&c, &c2, sizeof(c));

    vdm_evt_scan_done_t s = {.total = 5, .ds18_count = 4, .ds2438_count = 1, .bus_ok = 1}, s2;
    TEST_ASSERT_TRUE(vdm_enc_evt_scan_done(&s, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_evt_scan_done(buf, n, &s2));
    TEST_ASSERT_EQUAL_MEMORY(&s, &s2, sizeof(s));

    const char *msg = "valve 3: endstop not reached";
    vdm_evt_log_t l;
    TEST_ASSERT_TRUE(vdm_enc_evt_log(VDM_LOG_WARN, msg, strlen(msg), buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_evt_log(buf, n, &l));
    TEST_ASSERT_EQUAL_UINT8(VDM_LOG_WARN, l.level);
    TEST_ASSERT_EQUAL_STRING(msg, l.text);

    char longtext[VDM_LOG_TEXT_MAX + 1];
    memset(longtext, 'x', sizeof(longtext));
    TEST_ASSERT_TRUE(
        vdm_enc_evt_log(VDM_LOG_INFO, longtext, VDM_LOG_TEXT_MAX, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_evt_log(buf, n, &l));
    TEST_ASSERT_EQUAL_UINT8(VDM_LOG_TEXT_MAX, l.len);
    TEST_ASSERT_EQUAL_CHAR('\0', l.text[VDM_LOG_TEXT_MAX]);
    TEST_ASSERT_FALSE(
        vdm_enc_evt_log(VDM_LOG_INFO, longtext, VDM_LOG_TEXT_MAX + 1, buf, sizeof(buf), &n));
    TEST_ASSERT_FALSE(vdm_enc_evt_log(4, "x", 1, buf, sizeof(buf), &n));
}

static void test_encoders_respect_capacity(void)
{
    vdm_hello_t h = {0};
    TEST_ASSERT_FALSE(vdm_enc_hello(&h, buf, VDM_HELLO_LEN - 1, &n));
    TEST_ASSERT_TRUE(vdm_enc_hello(&h, buf, VDM_HELLO_LEN, &n));
    vdm_diag_t d = {0};
    TEST_ASSERT_FALSE(vdm_enc_diag(&d, buf, 10, &n));
}

static void test_config_crc(void)
{
    vdm_system_config_t s = vdm_system_config_default;
    vdm_motor_params_t m = vdm_motor_params_default;
    uint16_t a = vdm_config_crc(&s, &m);
    TEST_ASSERT_EQUAL_HEX16(a, vdm_config_crc(&s, &m));
    s.failsafe_pos = 500;
    TEST_ASSERT_NOT_EQUAL(a, vdm_config_crc(&s, &m));
    s = vdm_system_config_default;
    m.max_calib_retries = 3;
    TEST_ASSERT_NOT_EQUAL(a, vdm_config_crc(&s, &m));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_response_ok_with_body);
    RUN_TEST(test_response_errors);
    RUN_TEST(test_mask_channel_offset_u16);
    RUN_TEST(test_reset_magic);
    RUN_TEST(test_hello);
    RUN_TEST(test_system_config_roundtrip_and_defaults);
    RUN_TEST(test_system_config_ranges);
    RUN_TEST(test_motor_params);
    RUN_TEST(test_valve_state_spec_example);
    RUN_TEST(test_valve_state_validation);
    RUN_TEST(test_valve_states_list);
    RUN_TEST(test_set_targets);
    RUN_TEST(test_valve_detail);
    RUN_TEST(test_sensors);
    RUN_TEST(test_diag);
    RUN_TEST(test_events);
    RUN_TEST(test_encoders_respect_capacity);
    RUN_TEST(test_config_crc);
    return UNITY_END();
}
