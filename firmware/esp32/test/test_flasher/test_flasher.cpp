#include <string.h>
#include <unity.h>

#include "../support/fake_bootloader.h"
#include "core/stm32_flasher.h"

using sim::FakeBootloader;
using sim::makeImage;
using sim::MemImage;
using vdm::FlashResult;
using vdm::Stm32Flasher;

void setUp(void) {}
void tearDown(void) {}

static int last_percent;
static void progress(void *, uint8_t p, const char *)
{
    TEST_ASSERT_TRUE(p >= last_percent);
    last_percent = p;
}

static void test_flash_and_verify(void)
{
    FakeBootloader bl;
    MemImage img = makeImage(45 * 1024 + 13); /* not a multiple of the block size */
    Stm32Flasher f;
    last_percent = 0;
    TEST_ASSERT_EQUAL(FlashResult::Ok, f.flash(bl, img, progress, nullptr));
    TEST_ASSERT_EQUAL_HEX16(0x431, f.chipId());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(img.data.data(), bl.flash.data(), img.data.size());
    TEST_ASSERT_EQUAL_HEX8(0xFF, bl.flash[img.data.size()]); /* padding */
    TEST_ASSERT_EQUAL_HEX32(0x07, bl.sectors_erased);        /* 3 x 16 KiB only */
    TEST_ASSERT_EQUAL_INT(100, last_percent);
    /* back in the application, normal UART settings */
    TEST_ASSERT_FALSE(bl.boot0);
    TEST_ASSERT_FALSE(bl.reset);
    TEST_ASSERT_FALSE(bl.in_bootloader); /* application started again */
    TEST_ASSERT_EQUAL_UINT32(115200, bl.baud);
    TEST_ASSERT_FALSE(bl.parity);
}

static void test_software_entry_without_boot0(void)
{
    /* C2/C3 boards: BOOT0 is not wired, the application jumps to the
     * bootloader on request (legacy 1.x and v2 firmware) */
    FakeBootloader bl;
    bl.boot0_wired = false;
    MemImage img = makeImage(20000);
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::Ok, f.flash(bl, img));
    TEST_ASSERT_EQUAL_INT(1, bl.software_entries);
    TEST_ASSERT_NOT_NULL(strstr(f.trace(), "BEEFIT"));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(img.data.data(), bl.flash.data(), img.data.size());
}

static void test_already_in_bootloader(void)
{
    FakeBootloader bl;
    bl.boot0_wired = false;
    bl.app_present = false;
    bl.start_in_bootloader = true;
    bl.init();
    MemImage img = makeImage(8192);
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::Ok, f.flash(bl, img));
    TEST_ASSERT_EQUAL_INT(0, bl.software_entries);
}

static void test_boot0_fallback_for_blank_chip(void)
{
    /* no application (e.g. after an interrupted update): only BOOT0 helps */
    FakeBootloader bl;
    bl.app_present = false;
    MemImage img = makeImage(8192);
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::Ok, f.flash(bl, img));

    FakeBootloader unwired;
    unwired.app_present = false;
    unwired.boot0_wired = false;
    TEST_ASSERT_EQUAL(FlashResult::NoBootloader, f.flash(unwired, img));
    TEST_ASSERT_FALSE(unwired.boot0);
    TEST_ASSERT_NOT_NULL(strstr(f.trace(), "no BEEFIT"));
    TEST_ASSERT_NOT_NULL(strstr(f.trace(), "boot0: no answer"));
}

static void test_large_image_erases_more_sectors(void)
{
    FakeBootloader bl;
    MemImage img = makeImage(200 * 1024);
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::Ok, f.flash(bl, img));
    TEST_ASSERT_EQUAL_HEX32(0x3F, bl.sectors_erased); /* sectors 0..5 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(img.data.data(), bl.flash.data(), img.data.size());
}

static void test_bad_image_is_rejected_untouched(void)
{
    FakeBootloader bl;
    MemImage img = makeImage(4096);
    img.data[3] = 0x00; /* stack pointer not in SRAM */
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::BadImage, f.flash(bl, img));
    TEST_ASSERT_EQUAL_HEX32(0, bl.sectors_erased);
    TEST_ASSERT_FALSE(bl.boot0);
}

static void test_no_bootloader(void)
{
    FakeBootloader bl;
    bl.silent = true;
    MemImage img = makeImage(4096);
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::NoBootloader, f.flash(bl, img));
    TEST_ASSERT_FALSE(bl.boot0); /* application restarted */
    TEST_ASSERT_TRUE(bl.app_starts >= 1);
}

static void test_fallback_baud_rate(void)
{
    FakeBootloader bl;
    bl.app_present = false; /* BOOT0 path, which also tries 57600 baud */
    bl.accept_baud = 57600;
    MemImage img = makeImage(8192);
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::Ok, f.flash(bl, img));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(img.data.data(), bl.flash.data(), img.data.size());
}

static void test_write_retry_and_failure(void)
{
    FakeBootloader bl;
    bl.nack_writes = 2;
    MemImage img = makeImage(8192);
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::Ok, f.flash(bl, img));

    FakeBootloader bl2;
    bl2.nack_writes = 3;
    TEST_ASSERT_EQUAL(FlashResult::WriteFailed, f.flash(bl2, img));
}

static void test_verify_failure(void)
{
    FakeBootloader bl;
    bl.corrupt_read = true;
    MemImage img = makeImage(8192);
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::VerifyFailed, f.flash(bl, img));
}

static void test_chip_checks(void)
{
    FakeBootloader small(256 * 1024);
    small.chip_id = 0x423; /* F401CC, 256 KiB */
    MemImage img = makeImage(300 * 1024);
    Stm32Flasher f;
    TEST_ASSERT_EQUAL(FlashResult::TooLarge, f.flash(small, img));
    TEST_ASSERT_EQUAL_HEX32(0, small.sectors_erased);

    FakeBootloader other;
    other.chip_id = 0x410; /* F103 */
    MemImage img2 = makeImage(4096);
    TEST_ASSERT_EQUAL(FlashResult::UnknownChip, f.flash(other, img2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_flash_and_verify);
    RUN_TEST(test_software_entry_without_boot0);
    RUN_TEST(test_already_in_bootloader);
    RUN_TEST(test_boot0_fallback_for_blank_chip);
    RUN_TEST(test_large_image_erases_more_sectors);
    RUN_TEST(test_bad_image_is_rejected_untouched);
    RUN_TEST(test_no_bootloader);
    RUN_TEST(test_fallback_baud_rate);
    RUN_TEST(test_write_retry_and_failure);
    RUN_TEST(test_verify_failure);
    RUN_TEST(test_chip_checks);
    return UNITY_END();
}
