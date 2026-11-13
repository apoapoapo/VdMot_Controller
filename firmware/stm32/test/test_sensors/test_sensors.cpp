#include <string.h>
#include <unity.h>

#include "../support/fake_onewire.h"
#include "core/sensor_manager.h"

using sim::FakeBus;
using sim::makeDevice;
using vdm::SensorManager;
using vdm::onewire::kFamilyDs18b20;
using vdm::onewire::kFamilyDs2438;

namespace
{

uint32_t now;
uint32_t max_slots;

void run(SensorManager &s, FakeBus &bus, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        bus.slots = 0;
        s.tick(++now);
        if (bus.slots > max_slots) {
            max_slots = bus.slots;
        }
    }
}

FakeBus standardBus()
{
    FakeBus bus;
    for (uint32_t i = 0; i < 5; i++) {
        sim::OwDevice d = makeDevice(kFamilyDs18b20, 0x1000 + i * 37);
        d.temp_mc = 18000 + static_cast<int32_t>(i) * 1250;
        bus.devs.push_back(d);
    }
    sim::OwDevice v1 = makeDevice(kFamilyDs2438, 0x5000);
    v1.temp_mc = 23125;
    bus.devs.push_back(v1);
    sim::OwDevice v2 = makeDevice(kFamilyDs2438, 0x5001);
    v2.vad_mv = 3300;
    bus.devs.push_back(v2);
    bus.devs.push_back(makeDevice(0x10, 0x9999)); /* DS18S20: not supported, ignored */
    return bus;
}

int findEntry(SensorManager &s, const uint8_t *rom, uint8_t kind, vdm_sensor_value_t &out)
{
    for (uint8_t i = 0; i < s.entryCount(); i++) {
        s.entry(i, now, out);
        if (memcmp(out.rom, rom, 8) == 0 && out.kind == kind) {
            return i;
        }
    }
    return -1;
}

} // namespace

void setUp(void)
{
    now = 0;
    max_slots = 0;
}
void tearDown(void) {}

static void test_crc8(void)
{
    /* ROM example from Maxim application note 27 */
    const uint8_t rom[8] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xA2};
    TEST_ASSERT_EQUAL_HEX8(0xA2, vdm::onewire::crc8(rom, 7));
    TEST_ASSERT_EQUAL_HEX8(0x00, vdm::onewire::crc8(rom, 8));
}

static void test_search_finds_all_devices(void)
{
    FakeBus bus = standardBus();
    for (uint32_t i = 0; i < 25; i++)
        bus.devs.push_back(makeDevice(kFamilyDs18b20, 0x7000 + i * 11));
    vdm::onewire::Search search;
    search.restart();
    uint8_t rom[8];
    int found = 0;
    while (search.next(bus, rom)) {
        bool known = false;
        for (const sim::OwDevice &d : bus.devs)
            known = known || memcmp(d.rom, rom, 8) == 0;
        TEST_ASSERT_TRUE(known);
        found++;
    }
    TEST_ASSERT_EQUAL_INT((int)bus.devs.size(), found);
    TEST_ASSERT_FALSE(search.busError());
}

static void test_scan_and_measure(void)
{
    FakeBus bus = standardBus();
    SensorManager s(bus);
    run(s, bus, 50);
    vdm_evt_scan_done_t evt;
    TEST_ASSERT_TRUE(s.takeScanDone(evt));
    TEST_ASSERT_EQUAL_UINT8(7, evt.total);
    TEST_ASSERT_EQUAL_UINT8(5, evt.ds18_count);
    TEST_ASSERT_EQUAL_UINT8(2, evt.ds2438_count);
    TEST_ASSERT_EQUAL_UINT8(1, evt.bus_ok);
    TEST_ASSERT_EQUAL_UINT8(5 + 2 * 3, s.entryCount());

    vdm_sensor_value_t v;
    TEST_ASSERT_TRUE(s.entry(0, now, v));
    TEST_ASSERT_EQUAL_UINT8(VDM_SSTAT_NOT_MEASURED, v.status);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, v.age_s);

    run(s, bus, 1000);
    TEST_ASSERT_TRUE(s.takeCycleDone());
    for (uint32_t i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(findEntry(s, bus.devs[i].rom, VDM_SENSOR_TEMP_DS18B20, v) >= 0);
        TEST_ASSERT_EQUAL_UINT8(VDM_SSTAT_OK, v.status);
        TEST_ASSERT_EQUAL_INT32(bus.devs[i].temp_mc, v.value);
        TEST_ASSERT_TRUE(v.age_s <= 1);
    }
    TEST_ASSERT_TRUE(findEntry(s, bus.devs[5].rom, VDM_SENSOR_TEMP_DS2438, v) >= 0);
    TEST_ASSERT_EQUAL_INT32(23125, v.value);
    TEST_ASSERT_TRUE(findEntry(s, bus.devs[5].rom, VDM_SENSOR_VAD_DS2438, v) >= 0);
    TEST_ASSERT_EQUAL_INT32(1230, v.value);
    TEST_ASSERT_TRUE(findEntry(s, bus.devs[5].rom, VDM_SENSOR_VDD_DS2438, v) >= 0);
    TEST_ASSERT_EQUAL_INT32(4980, v.value);
    TEST_ASSERT_TRUE(findEntry(s, bus.devs[6].rom, VDM_SENSOR_VAD_DS2438, v) >= 0);
    TEST_ASSERT_EQUAL_INT32(3300, v.value);
    TEST_ASSERT_EQUAL_UINT16(0, s.crcErrors());

    /* a single tick never occupies the bus for long (~65 us per slot) */
    TEST_ASSERT_TRUE(max_slots <= SensorManager::kMaxSlotsPerTick);
}

static void test_negative_temperature(void)
{
    FakeBus bus;
    sim::OwDevice d = makeDevice(kFamilyDs18b20, 1);
    d.temp_mc = -10250;
    bus.devs.push_back(d);
    SensorManager s(bus);
    run(s, bus, 1000);
    vdm_sensor_value_t v;
    TEST_ASSERT_TRUE(s.entry(0, now, v));
    TEST_ASSERT_EQUAL_INT32(-10250, v.value);
}

static void test_errors_keep_last_value(void)
{
    FakeBus bus = standardBus();
    SensorManager s(bus);
    s.setInterval(10);
    run(s, bus, 1500);
    bus.devs[1].corrupt = true;
    bus.devs[2].present = false;
    bus.devs[3].ignore_convert = true;
    bus.devs[3].scratch_raw = 0x0550; /* power-on value */
    run(s, bus, 12000);

    vdm_sensor_value_t v;
    TEST_ASSERT_TRUE(findEntry(s, bus.devs[1].rom, VDM_SENSOR_TEMP_DS18B20, v) >= 0);
    TEST_ASSERT_EQUAL_UINT8(VDM_SSTAT_CRC_ERROR, v.status);
    TEST_ASSERT_EQUAL_INT32(bus.devs[1].temp_mc, v.value);
    TEST_ASSERT_TRUE(v.age_s >= 10);
    TEST_ASSERT_TRUE(findEntry(s, bus.devs[2].rom, VDM_SENSOR_TEMP_DS18B20, v) >= 0);
    TEST_ASSERT_EQUAL_UINT8(VDM_SSTAT_UNREACHABLE, v.status);
    TEST_ASSERT_TRUE(findEntry(s, bus.devs[3].rom, VDM_SENSOR_TEMP_DS18B20, v) >= 0);
    TEST_ASSERT_EQUAL_UINT8(VDM_SSTAT_OUT_OF_RANGE, v.status);
    TEST_ASSERT_TRUE(findEntry(s, bus.devs[0].rom, VDM_SENSOR_TEMP_DS18B20, v) >= 0);
    TEST_ASSERT_EQUAL_UINT8(VDM_SSTAT_OK, v.status);
    TEST_ASSERT_TRUE(s.crcErrors() >= 1);

    /* the removed sensor disappears with the next scan */
    TEST_ASSERT_TRUE(s.startScan());
    TEST_ASSERT_FALSE(s.startScan()); /* already requested */
    run(s, bus, 100);
    vdm_evt_scan_done_t evt;
    TEST_ASSERT_TRUE(s.takeScanDone(evt));
    TEST_ASSERT_EQUAL_UINT8(6, evt.total);
    TEST_ASSERT_EQUAL_INT(-1, findEntry(s, bus.devs[2].rom, VDM_SENSOR_TEMP_DS18B20, v));
    /* values of the remaining devices survive the rescan */
    TEST_ASSERT_TRUE(findEntry(s, bus.devs[0].rom, VDM_SENSOR_TEMP_DS18B20, v) >= 0);
    TEST_ASSERT_EQUAL_UINT8(VDM_SSTAT_OK, v.status);
    TEST_ASSERT_EQUAL_INT32(bus.devs[0].temp_mc, v.value);
}

static void test_measurement_interval(void)
{
    FakeBus bus = standardBus();
    SensorManager s(bus);
    s.setInterval(30);
    int cycles = 0;
    for (int i = 0; i < 100; i++) {
        run(s, bus, 1000);
        cycles += s.takeCycleDone();
    }
    TEST_ASSERT_EQUAL_INT(4, cycles); /* t = 0, 30, 60, 90 s */
}

static void test_short_and_empty_bus(void)
{
    FakeBus bus;
    bus.shorted = true;
    SensorManager s(bus);
    run(s, bus, 100);
    vdm_evt_scan_done_t evt;
    TEST_ASSERT_TRUE(s.takeScanDone(evt));
    TEST_ASSERT_EQUAL_UINT8(0, evt.total);
    TEST_ASSERT_EQUAL_UINT8(0, evt.bus_ok);
    TEST_ASSERT_TRUE(s.busErrors() >= 1);

    FakeBus empty;
    SensorManager s2(empty);
    run(s2, empty, 2000);
    TEST_ASSERT_TRUE(s2.takeScanDone(evt));
    TEST_ASSERT_EQUAL_UINT8(0, evt.total);
    TEST_ASSERT_EQUAL_UINT8(1, evt.bus_ok);
    TEST_ASSERT_EQUAL_UINT8(0, s2.entryCount());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc8);
    RUN_TEST(test_search_finds_all_devices);
    RUN_TEST(test_scan_and_measure);
    RUN_TEST(test_negative_temperature);
    RUN_TEST(test_errors_keep_last_value);
    RUN_TEST(test_measurement_interval);
    RUN_TEST(test_short_and_empty_bus);
    return UNITY_END();
}
