#include <unity.h>

#include "../support/fake_eeprom.h"
#include "core/storage.h"

using sim::FakeEeprom;
using vdm::ChannelPersist;
using vdm::Counters;
using vdm::Storage;
using vdm::SystemData;

void setUp(void) {}
void tearDown(void) {}

static ChannelPersist sampleChannel(uint32_t seed)
{
    ChannelPersist p;
    p.open_counts = 13754 + seed;
    p.close_counts = 10907 + seed;
    p.mean_open_ma10 = 210;
    p.mean_close_ma10 = 190;
    p.calib_op_hours = 1000 + seed;
    p.moves_since_calib = 17;
    p.moves_total = 123456 + seed;
    p.calibrated = 1;
    return p;
}

static void test_empty_eeprom_has_no_records(void)
{
    FakeEeprom e;
    Storage s(e);
    SystemData sd;
    Counters c;
    uint16_t t[12];
    ChannelPersist p;
    TEST_ASSERT_FALSE(s.loadSystem(sd));
    TEST_ASSERT_FALSE(s.loadCounters(c));
    TEST_ASSERT_FALSE(s.loadTargets(t));
    TEST_ASSERT_FALSE(s.loadChannel(3, p));
    TEST_ASSERT_TRUE(s.idle());
}

static void test_roundtrip_all_records(void)
{
    FakeEeprom e;
    e.busy_polls = 3;
    {
        Storage s(e);
        SystemData sd{vdm_system_config_default, vdm_motor_params_default, 0x0A55};
        sd.sys.failsafe_pos = 650;
        sd.motor.max_calib_retries = 4;
        s.saveSystem(sd);
        s.saveCounters({42, 9000});
        uint16_t t[12];
        for (int i = 0; i < 12; i++)
            t[i] = (uint16_t)(i * 80);
        s.saveTargets(t);
        for (uint8_t ch = 0; ch < 12; ch++)
            s.saveChannel(ch, sampleChannel(ch));
        TEST_ASSERT_FALSE(s.idle());
        for (int i = 0; i < 10000 && !s.idle(); i++)
            s.tick();
        TEST_ASSERT_TRUE(s.idle());
        TEST_ASSERT_EQUAL_UINT32(15, s.writes());
        TEST_ASSERT_EQUAL_UINT16(0, s.errors());
    }
    TEST_ASSERT_EQUAL_UINT32(0, e.violations);

    Storage s2(e);
    SystemData sd;
    TEST_ASSERT_TRUE(s2.loadSystem(sd));
    TEST_ASSERT_EQUAL_UINT16(650, sd.sys.failsafe_pos);
    TEST_ASSERT_EQUAL_UINT8(4, sd.motor.max_calib_retries);
    TEST_ASSERT_EQUAL_HEX16(0x0A55, sd.enabled_mask);
    Counters c;
    TEST_ASSERT_TRUE(s2.loadCounters(c));
    TEST_ASSERT_EQUAL_UINT32(42, c.boot_count);
    TEST_ASSERT_EQUAL_UINT32(9000, c.op_hours);
    uint16_t t[12];
    TEST_ASSERT_TRUE(s2.loadTargets(t));
    TEST_ASSERT_EQUAL_UINT16(880, t[11]);
    for (uint8_t ch = 0; ch < 12; ch++) {
        ChannelPersist p;
        TEST_ASSERT_TRUE(s2.loadChannel(ch, p));
        ChannelPersist x = sampleChannel(ch);
        TEST_ASSERT_EQUAL_UINT32(x.open_counts, p.open_counts);
        TEST_ASSERT_EQUAL_UINT32(x.close_counts, p.close_counts);
        TEST_ASSERT_EQUAL_UINT32(x.moves_total, p.moves_total);
        TEST_ASSERT_EQUAL_UINT32(x.calib_op_hours, p.calib_op_hours);
        TEST_ASSERT_EQUAL_UINT16(x.mean_close_ma10, p.mean_close_ma10);
        TEST_ASSERT_EQUAL_UINT8(1, p.calibrated);
    }
}

static void test_newest_copy_wins_and_slots_alternate(void)
{
    FakeEeprom e;
    Storage s(e);
    for (uint32_t i = 1; i <= 300; i++) { /* sequence numbers wrap */
        s.saveCounters({i, i * 2});
        s.flush();
        Storage r(e);
        Counters c;
        TEST_ASSERT_TRUE(r.loadCounters(c));
        TEST_ASSERT_EQUAL_UINT32(i, c.boot_count);
    }
}

static void test_power_loss_during_write_keeps_previous_copy(void)
{
    FakeEeprom e;
    {
        Storage s(e);
        s.saveChannel(5, sampleChannel(1));
        s.flush();
    }
    /* single-page record: power lost in the middle of the page */
    e.power_loss_at = (int32_t)e.page_writes;
    {
        Storage s(e);
        ChannelPersist p;
        TEST_ASSERT_TRUE(s.loadChannel(5, p));
        s.saveChannel(5, sampleChannel(2));
        s.flush();
    }
    e.powerCycle();
    {
        Storage r(e);
        ChannelPersist p;
        TEST_ASSERT_TRUE(r.loadChannel(5, p));
        TEST_ASSERT_EQUAL_UINT32(sampleChannel(1).open_counts, p.open_counts);
        /* a complete write afterwards wins */
        r.saveChannel(5, sampleChannel(3));
        r.flush();
    }
    Storage r(e);
    ChannelPersist p;
    TEST_ASSERT_TRUE(r.loadChannel(5, p));
    TEST_ASSERT_EQUAL_UINT32(sampleChannel(3).open_counts, p.open_counts);
}

static void test_power_loss_in_multi_page_record(void)
{
    FakeEeprom e;
    SystemData sd{vdm_system_config_default, vdm_motor_params_default, 0x0001};
    {
        Storage s(e);
        s.saveSystem(sd);
        s.flush();
    }
    for (int cut = 0; cut < 2; cut++) { /* first or second page of the record */
        e.power_loss_at = (int32_t)e.page_writes + cut;
        Storage s(e);
        SystemData cur;
        TEST_ASSERT_TRUE(s.loadSystem(cur));
        SystemData next = sd;
        next.enabled_mask = 0x0FFF;
        s.saveSystem(next);
        s.flush();
        e.powerCycle();
        Storage r(e);
        TEST_ASSERT_TRUE(r.loadSystem(cur));
        TEST_ASSERT_EQUAL_HEX16(0x0001, cur.enabled_mask);
    }
}

static void test_corrupted_copy_falls_back(void)
{
    FakeEeprom e;
    Storage s(e);
    s.saveTargets({100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100});
    s.flush();
    s.saveTargets({200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200});
    s.flush();
    /* flip a bit in the newer copy (slot A, written second) */
    e.mem[0x01C0 + 6] ^= 0x01;
    Storage r(e);
    uint16_t t[12];
    TEST_ASSERT_TRUE(r.loadTargets(t));
    TEST_ASSERT_EQUAL_UINT16(100, t[0]);
}

static void test_write_errors_are_counted(void)
{
    FakeEeprom e;
    e.fail_after = 0;
    Storage s(e);
    s.saveCounters({1, 1});
    s.flush();
    TEST_ASSERT_TRUE(s.idle());
    TEST_ASSERT_EQUAL_UINT16(1, s.errors());
    TEST_ASSERT_EQUAL_UINT32(0, s.writes());
}

static void test_invalid_system_config_is_not_stored(void)
{
    FakeEeprom e;
    Storage s(e);
    SystemData sd{vdm_system_config_default, vdm_motor_params_default, 0x0FFF};
    sd.sys.sensor_interval_s = 1; /* out of range */
    s.saveSystem(sd);
    s.flush();
    TEST_ASSERT_EQUAL_UINT32(0, s.writes());
}

static void test_layout_does_not_overlap(void)
{
    FakeEeprom e;
    Storage s(e);
    SystemData sd{vdm_system_config_default, vdm_motor_params_default, 0x0FFF};
    /* write everything twice so both slots of every record are used */
    for (int k = 0; k < 2; k++) {
        s.saveSystem(sd);
        s.saveCounters({7, 8});
        s.saveTargets({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
        for (uint8_t ch = 0; ch < 12; ch++)
            s.saveChannel(ch, sampleChannel(ch));
        s.flush();
    }
    Storage r(e);
    TEST_ASSERT_TRUE(r.loadSystem(sd));
    Counters c;
    TEST_ASSERT_TRUE(r.loadCounters(c));
    uint16_t t[12];
    TEST_ASSERT_TRUE(r.loadTargets(t));
    TEST_ASSERT_EQUAL_UINT16(12, t[11]);
    for (uint8_t ch = 0; ch < 12; ch++) {
        ChannelPersist p;
        TEST_ASSERT_TRUE(r.loadChannel(ch, p));
        TEST_ASSERT_EQUAL_UINT32(sampleChannel(ch).moves_total, p.moves_total);
    }
    /* nothing below 0x0100 (legacy area) and nothing above 0x0500 */
    for (int a = 0; a < 0x100; a++)
        TEST_ASSERT_EQUAL_HEX8(0xFF, e.mem[a]);
    for (int a = 0x500; a < 8192; a++)
        TEST_ASSERT_EQUAL_HEX8(0xFF, e.mem[a]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_eeprom_has_no_records);
    RUN_TEST(test_roundtrip_all_records);
    RUN_TEST(test_newest_copy_wins_and_slots_alternate);
    RUN_TEST(test_power_loss_during_write_keeps_previous_copy);
    RUN_TEST(test_power_loss_in_multi_page_record);
    RUN_TEST(test_corrupted_copy_falls_back);
    RUN_TEST(test_write_errors_are_counted);
    RUN_TEST(test_invalid_system_config_is_not_stored);
    RUN_TEST(test_layout_does_not_overlap);
    return UNITY_END();
}
