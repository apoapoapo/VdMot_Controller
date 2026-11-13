#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "vdm/cobs.h"

void setUp(void) {}
void tearDown(void) {}

static void check_vector(const uint8_t *raw, size_t raw_len, const uint8_t *enc, size_t enc_len)
{
    uint8_t buf[600];
    size_t n = 0;

    TEST_ASSERT_TRUE(vdm_cobs_encode(raw, raw_len, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(enc_len, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(enc, buf, enc_len);

    uint8_t dec[600];
    TEST_ASSERT_TRUE(vdm_cobs_decode(enc, enc_len, dec, sizeof(dec), &n));
    TEST_ASSERT_EQUAL_size_t(raw_len, n);
    if (raw_len > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, dec, raw_len);
    }
}

/* Reference vectors from the COBS paper / Wikipedia */
static void test_vectors_short(void)
{
    {
        const uint8_t e[] = {0x01};
        check_vector(NULL, 0, e, sizeof(e));
    }
    {
        const uint8_t r[] = {0x00}, e[] = {0x01, 0x01};
        check_vector(r, sizeof(r), e, sizeof(e));
    }
    {
        const uint8_t r[] = {0x00, 0x00}, e[] = {0x01, 0x01, 0x01};
        check_vector(r, sizeof(r), e, sizeof(e));
    }
    {
        const uint8_t r[] = {0x00, 0x11, 0x00}, e[] = {0x01, 0x02, 0x11, 0x01};
        check_vector(r, sizeof(r), e, sizeof(e));
    }
    {
        const uint8_t r[] = {0x11, 0x22, 0x00, 0x33}, e[] = {0x03, 0x11, 0x22, 0x02, 0x33};
        check_vector(r, sizeof(r), e, sizeof(e));
    }
    {
        const uint8_t r[] = {0x11, 0x22, 0x33, 0x44}, e[] = {0x05, 0x11, 0x22, 0x33, 0x44};
        check_vector(r, sizeof(r), e, sizeof(e));
    }
    {
        const uint8_t r[] = {0x11, 0x00, 0x00, 0x00}, e[] = {0x02, 0x11, 0x01, 0x01, 0x01};
        check_vector(r, sizeof(r), e, sizeof(e));
    }
}

static void test_vectors_long_blocks(void)
{
    uint8_t raw[256], enc[260];

    /* 01..FE (254 bytes) -> FF 01..FE */
    for (int i = 0; i < 254; i++)
        raw[i] = (uint8_t)(i + 1);
    enc[0] = 0xFF;
    memcpy(&enc[1], raw, 254);
    check_vector(raw, 254, enc, 255);

    /* 00 01..FE (255 bytes) -> 01 FF 01..FE */
    raw[0] = 0x00;
    for (int i = 1; i < 255; i++)
        raw[i] = (uint8_t)i;
    enc[0] = 0x01;
    enc[1] = 0xFF;
    memcpy(&enc[2], &raw[1], 254);
    check_vector(raw, 255, enc, 256);

    /* 01..FF (255 bytes) -> FF 01..FE 02 FF */
    for (int i = 0; i < 255; i++)
        raw[i] = (uint8_t)(i + 1);
    enc[0] = 0xFF;
    memcpy(&enc[1], raw, 254);
    enc[255] = 0x02;
    enc[256] = 0xFF;
    check_vector(raw, 255, enc, 257);

    /* 02..FF 00 (255 bytes) -> FF 02..FF 01 01 */
    for (int i = 0; i < 254; i++)
        raw[i] = (uint8_t)(i + 2);
    raw[254] = 0x00;
    enc[0] = 0xFF;
    memcpy(&enc[1], raw, 254);
    enc[255] = 0x01;
    enc[256] = 0x01;
    check_vector(raw, 255, enc, 257);
}

static void test_roundtrip_random(void)
{
    srand(12345);
    for (int iter = 0; iter < 2000; iter++) {
        uint8_t raw[300], enc[VDM_COBS_MAX_ENCODED(300)], dec[300];
        size_t len = (size_t)(rand() % 300);
        for (size_t i = 0; i < len; i++) {
            /* bias towards zeros */
            raw[i] = (rand() % 4 == 0) ? 0 : (uint8_t)rand();
        }
        size_t n, m;
        TEST_ASSERT_TRUE(vdm_cobs_encode(raw, len, enc, sizeof(enc), &n));
        TEST_ASSERT_TRUE(n <= VDM_COBS_MAX_ENCODED(len));
        for (size_t i = 0; i < n; i++) {
            TEST_ASSERT_NOT_EQUAL(0, enc[i]);
        }
        TEST_ASSERT_TRUE(vdm_cobs_decode(enc, n, dec, sizeof(dec), &m));
        TEST_ASSERT_EQUAL_size_t(len, m);
        if (len > 0) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, dec, len);
        }
    }
}

static void test_encode_capacity(void)
{
    const uint8_t r[] = {0x11, 0x22};
    uint8_t out[3];
    size_t n;
    TEST_ASSERT_TRUE(vdm_cobs_encode(r, 2, out, 3, &n));
    TEST_ASSERT_FALSE(vdm_cobs_encode(r, 2, out, 2, &n));
}

static void test_decode_malformed(void)
{
    uint8_t out[16];
    size_t n;
    const uint8_t zero_code[] = {0x00};
    const uint8_t embedded_zero[] = {0x03, 0x11, 0x00};
    const uint8_t too_long[] = {0x05, 0x11, 0x22};

    TEST_ASSERT_FALSE(vdm_cobs_decode(zero_code, 0, out, sizeof(out), &n)); /* empty */
    TEST_ASSERT_FALSE(vdm_cobs_decode(zero_code, 1, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(vdm_cobs_decode(embedded_zero, 3, out, sizeof(out), &n));
    TEST_ASSERT_FALSE(vdm_cobs_decode(too_long, 3, out, sizeof(out), &n));
}

static void test_decode_capacity(void)
{
    const uint8_t e[] = {0x03, 0x11, 0x22, 0x02, 0x33}; /* 4 bytes decoded */
    uint8_t out[4];
    size_t n;
    TEST_ASSERT_TRUE(vdm_cobs_decode(e, sizeof(e), out, 4, &n));
    TEST_ASSERT_FALSE(vdm_cobs_decode(e, sizeof(e), out, 3, &n));
    TEST_ASSERT_FALSE(vdm_cobs_decode(e, sizeof(e), out, 2, &n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_vectors_short);
    RUN_TEST(test_vectors_long_blocks);
    RUN_TEST(test_roundtrip_random);
    RUN_TEST(test_encode_capacity);
    RUN_TEST(test_decode_malformed);
    RUN_TEST(test_decode_capacity);
    return UNITY_END();
}
