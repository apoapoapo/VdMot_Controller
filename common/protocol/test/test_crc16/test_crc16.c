#include <unity.h>

#include "vdm/crc16.h"

void setUp(void) {}
void tearDown(void) {}

static void test_check_value(void)
{
    const uint8_t data[] = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x29B1, vdm_crc16(data, 9));
}

static void test_empty_is_init(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, vdm_crc16(NULL, 0));
}

static void test_incremental_equals_oneshot(void)
{
    const uint8_t data[] = "123456789";
    uint16_t crc = vdm_crc16_update(VDM_CRC16_INIT, data, 4);
    crc = vdm_crc16_update(crc, &data[4], 5);
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_empty_is_init);
    RUN_TEST(test_incremental_equals_oneshot);
    return UNITY_END();
}
