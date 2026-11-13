#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "vdm/cobs.h"
#include "vdm/crc16.h"
#include "vdm/frame.h"

static vdm_rx_t rx;

void setUp(void)
{
    vdm_rx_init(&rx);
}
void tearDown(void) {}

/* Feeds `n` bytes, returns the number of frames received; the last one is
 * copied to `last` / `last_payload`. */
static vdm_frame_t last;
static uint8_t last_payload[VDM_PAYLOAD_MAX];

static int feed(const uint8_t *p, size_t n)
{
    int frames = 0;
    vdm_frame_t f;
    for (size_t i = 0; i < n; i++) {
        if (vdm_rx_feed(&rx, p[i], &f)) {
            frames++;
            last = f;
            memcpy(last_payload, f.payload, f.len);
            last.payload = last_payload;
        }
    }
    return frames;
}

static size_t make(uint8_t kind, uint8_t seq, uint8_t msg, const uint8_t *pl, size_t len,
                   uint8_t *out)
{
    size_t n = 0;
    TEST_ASSERT_TRUE(vdm_frame_encode(kind, seq, msg, pl, len, out, VDM_WIRE_MAX, &n));
    return n;
}

/* Example from the specification, chapter 8 */
static void test_spec_example_set_targets(void)
{
    const uint8_t payload[] = {0x01, 0x03, 0xC2, 0x01};
    uint8_t wire[VDM_WIRE_MAX];
    size_t n = make(VDM_KIND_REQUEST, 17, 0x11, payload, sizeof(payload), wire);

    /* decoded frame: 01 01 11 11 01 03 C2 01 crc_lo crc_hi */
    uint8_t raw[] = {0x01, 0x01, 0x11, 0x11, 0x01, 0x03, 0xC2, 0x01, 0, 0};
    uint16_t crc = vdm_crc16(raw, 8);
    raw[8] = (uint8_t)(crc & 0xFF);
    raw[9] = (uint8_t)(crc >> 8);
    uint8_t enc[16];
    size_t en;
    TEST_ASSERT_TRUE(vdm_cobs_encode(raw, sizeof(raw), enc, sizeof(enc), &en));
    TEST_ASSERT_EQUAL_size_t(en + 1, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(enc, wire, en);
    TEST_ASSERT_EQUAL_HEX8(0x00, wire[n - 1]);

    TEST_ASSERT_EQUAL_INT(1, feed(wire, n));
    TEST_ASSERT_EQUAL_UINT8(VDM_KIND_REQUEST, last.kind);
    TEST_ASSERT_EQUAL_UINT8(17, last.seq);
    TEST_ASSERT_EQUAL_UINT8(VDM_MSG_SET_TARGETS, last.msg);
    TEST_ASSERT_EQUAL_size_t(sizeof(payload), last.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, last.payload, sizeof(payload));
}

static void test_empty_and_max_payload(void)
{
    uint8_t wire[VDM_WIRE_MAX];
    size_t n = make(VDM_KIND_RESPONSE, 1, VDM_MSG_PING, NULL, 0, wire);
    TEST_ASSERT_EQUAL_INT(1, feed(wire, n));
    TEST_ASSERT_EQUAL_size_t(0, last.len);

    uint8_t pl[VDM_PAYLOAD_MAX];
    for (size_t i = 0; i < sizeof(pl); i++)
        pl[i] = (uint8_t)i; /* includes zeros */
    n = make(VDM_KIND_EVENT, 255, VDM_EVT_LOG, pl, sizeof(pl), wire);
    TEST_ASSERT_TRUE(n <= VDM_WIRE_MAX);
    TEST_ASSERT_EQUAL_INT(1, feed(wire, n));
    TEST_ASSERT_EQUAL_size_t(VDM_PAYLOAD_MAX, last.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pl, last.payload, sizeof(pl));
}

static void test_encode_rejects(void)
{
    uint8_t wire[VDM_WIRE_MAX], pl[VDM_PAYLOAD_MAX + 1] = {0};
    size_t n;
    TEST_ASSERT_FALSE(vdm_frame_encode(VDM_KIND_REQUEST, 0, 1, NULL, 0, wire, sizeof(wire), &n));
    TEST_ASSERT_FALSE(
        vdm_frame_encode(VDM_KIND_REQUEST, 1, 1, pl, sizeof(pl), wire, sizeof(wire), &n));
    TEST_ASSERT_FALSE(vdm_frame_encode(VDM_KIND_REQUEST, 1, 1, pl, 4, wire, 5, &n));
}

static void test_back_to_back_frames(void)
{
    uint8_t stream[3 * VDM_WIRE_MAX];
    size_t n = 0;
    const uint8_t a[] = {1, 2, 3}, b[] = {0, 0}, c[] = {9};
    n += make(VDM_KIND_EVENT, 1, VDM_EVT_VALVE, a, sizeof(a), &stream[n]);
    n += make(VDM_KIND_EVENT, 2, VDM_EVT_VALVE, b, sizeof(b), &stream[n]);
    n += make(VDM_KIND_EVENT, 3, VDM_EVT_VALVE, c, sizeof(c), &stream[n]);
    TEST_ASSERT_EQUAL_INT(3, feed(stream, n));
    TEST_ASSERT_EQUAL_UINT8(3, last.seq);
    TEST_ASSERT_EQUAL_UINT32(3, rx.stats.frames_ok);
}

static void test_extra_delimiters_ignored(void)
{
    uint8_t wire[VDM_WIRE_MAX + 4] = {0, 0};
    size_t n = make(VDM_KIND_REQUEST, 5, VDM_MSG_HELLO, NULL, 0, &wire[2]);
    wire[2 + n] = 0;
    TEST_ASSERT_EQUAL_INT(1, feed(wire, n + 3));
    TEST_ASSERT_EQUAL_UINT16(0, rx.stats.malformed);
}

static void test_crc_error_and_resync(void)
{
    uint8_t wire[2 * VDM_WIRE_MAX];
    const uint8_t pl[] = {0x10, 0x20, 0x30};
    size_t n1 = make(VDM_KIND_REQUEST, 7, VDM_MSG_PING, pl, sizeof(pl), wire);
    wire[2] ^= 0x02; /* flip a bit of the kind byte (0x01 -> 0x03), stays non-zero */
    size_t n2 = make(VDM_KIND_REQUEST, 8, VDM_MSG_PING, pl, sizeof(pl), &wire[n1]);
    TEST_ASSERT_EQUAL_INT(1, feed(wire, n1 + n2));
    TEST_ASSERT_EQUAL_UINT8(8, last.seq);
    TEST_ASSERT_EQUAL_UINT16(1, rx.stats.crc_err);
}

static void test_garbage_then_frame(void)
{
    uint8_t buf[64 + VDM_WIRE_MAX];
    for (int i = 0; i < 64; i++)
        buf[i] = (uint8_t)(0x55 + i); /* no zero */
    buf[63] = 0x00;
    size_t n = make(VDM_KIND_REQUEST, 9, VDM_MSG_PING, NULL, 0, &buf[64]);
    TEST_ASSERT_EQUAL_INT(1, feed(buf, 64 + n));
    TEST_ASSERT_EQUAL_UINT8(9, last.seq);
    TEST_ASSERT_TRUE(rx.stats.malformed + rx.stats.crc_err >= 1);
}

static void test_overflow(void)
{
    uint8_t junk[VDM_ENCODED_MAX + 10];
    memset(junk, 0x42, sizeof(junk));
    TEST_ASSERT_EQUAL_INT(0, feed(junk, sizeof(junk)));
    uint8_t wire[VDM_WIRE_MAX];
    size_t n = make(VDM_KIND_REQUEST, 3, VDM_MSG_PING, NULL, 0, wire);
    /* the first delimiter (end of the frame) only terminates the junk */
    TEST_ASSERT_EQUAL_INT(0, feed(wire, n));
    TEST_ASSERT_EQUAL_UINT16(1, rx.stats.overflow);
    TEST_ASSERT_EQUAL_INT(1, feed(wire, n));
    TEST_ASSERT_EQUAL_UINT8(3, last.seq);
}

static void send_raw(const uint8_t *raw, size_t len)
{
    uint8_t enc[VDM_WIRE_MAX];
    size_t n;
    TEST_ASSERT_TRUE(vdm_cobs_encode(raw, len, enc, sizeof(enc) - 1, &n));
    enc[n++] = 0;
    TEST_ASSERT_EQUAL_INT(0, feed(enc, n));
}

static void with_crc(uint8_t *raw, size_t len_without_crc)
{
    uint16_t crc = vdm_crc16(raw, len_without_crc);
    raw[len_without_crc] = (uint8_t)(crc & 0xFF);
    raw[len_without_crc + 1] = (uint8_t)(crc >> 8);
}

static void test_reject_bad_header_fields(void)
{
    uint8_t raw[8];

    raw[0] = 2;
    raw[1] = VDM_KIND_REQUEST;
    raw[2] = 1;
    raw[3] = 1;
    with_crc(raw, 4);
    send_raw(raw, 6);
    TEST_ASSERT_EQUAL_UINT16(1, rx.stats.bad_version);

    raw[0] = 1;
    raw[1] = 0x04;
    with_crc(raw, 4);
    send_raw(raw, 6);
    TEST_ASSERT_EQUAL_UINT16(1, rx.stats.malformed);

    raw[1] = VDM_KIND_REQUEST;
    raw[2] = 0; /* seq 0 is reserved */
    with_crc(raw, 4);
    send_raw(raw, 6);
    TEST_ASSERT_EQUAL_UINT16(2, rx.stats.malformed);

    /* too short: 5 bytes */
    send_raw(raw, 5);
    TEST_ASSERT_EQUAL_UINT16(3, rx.stats.malformed);
    TEST_ASSERT_EQUAL_UINT32(0, rx.stats.frames_ok);
}

static void test_random_corruption_never_crashes(void)
{
    srand(777);
    uint8_t wire[VDM_WIRE_MAX];
    for (int iter = 0; iter < 3000; iter++) {
        uint8_t pl[40];
        size_t len = (size_t)(rand() % 40);
        for (size_t i = 0; i < len; i++)
            pl[i] = (uint8_t)rand();
        size_t n = make(VDM_KIND_EVENT, (uint8_t)(1 + rand() % 255), VDM_EVT_LOG, pl, len, wire);
        /* corrupt one random byte (may insert a delimiter) */
        wire[(size_t)rand() % n] = (uint8_t)rand();
        feed(wire, n);
        feed((const uint8_t[]){0x00}, 1);
    }
    /* afterwards the receiver must still work */
    size_t n = make(VDM_KIND_EVENT, 42, VDM_EVT_LOG, NULL, 0, wire);
    TEST_ASSERT_EQUAL_INT(1, feed(wire, n));
    TEST_ASSERT_EQUAL_UINT8(42, last.seq);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_spec_example_set_targets);
    RUN_TEST(test_empty_and_max_payload);
    RUN_TEST(test_encode_rejects);
    RUN_TEST(test_back_to_back_frames);
    RUN_TEST(test_extra_delimiters_ignored);
    RUN_TEST(test_crc_error_and_resync);
    RUN_TEST(test_garbage_then_frame);
    RUN_TEST(test_overflow);
    RUN_TEST(test_reject_bad_header_fields);
    RUN_TEST(test_random_corruption_never_crashes);
    return UNITY_END();
}
