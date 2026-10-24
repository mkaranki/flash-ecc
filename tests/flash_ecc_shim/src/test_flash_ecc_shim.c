/*
 * Unit tests for the ECC flash shim's per-event stats counters.
 *
 * Injects real bit-flip corruption directly on the parent flash device,
 * the same way `ecc_test inject` does on hardware, then reads back
 * through the shim and checks the flash_ecc CONFIG_STATS counters.
 *
 * Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/stats/stats.h>
#include <zephyr/sys/util.h>

#include <flash_ecc_shim.h>

#define DATA_SIZE   252U
#define SECTOR_SIZE (DATA_SIZE * 16U) /* virtual sector = data-size * pages-per-sector */

static const struct device *shim_dev = DEVICE_DT_GET(DT_NODELABEL(ecc_flash));
static const struct device *parent_dev;

struct find_stat_ctx {
	const char *name;
	uint32_t value;
	bool found;
};

static int find_stat_cb(struct stats_hdr *hdr, void *arg, const char *name, uint16_t off)
{
	struct find_stat_ctx *ctx = arg;

	if (strcmp(name, ctx->name) == 0) {
		ctx->value = *(uint32_t *)((uint8_t *)hdr + off);
		ctx->found = true;
		return 1;
	}
	return 0;
}

static uint32_t read_stat(const char *entry_name)
{
	struct stats_hdr *hdr = stats_group_find("flash_ecc");
	struct find_stat_ctx ctx = {.name = entry_name};

	zassert_not_null(hdr, "flash_ecc stats group not registered");
	stats_walk(hdr, find_stat_cb, &ctx);
	zassert_true(ctx.found, "stat entry %s not found", entry_name);
	return ctx.value;
}

#if !defined(CONFIG_ECC_CRC32_2BIT_CORRECTION)
static bool stat_exists(const char *entry_name)
{
	struct stats_hdr *hdr = stats_group_find("flash_ecc");
	struct find_stat_ctx ctx = {.name = entry_name};

	if (hdr == NULL) {
		return false;
	}
	stats_walk(hdr, find_stat_cb, &ctx);
	return ctx.found;
}
#endif /* !CONFIG_ECC_CRC32_2BIT_CORRECTION */

/*
 * Write one clean, known page (all 0xAA) through the shim, then corrupt
 * `flip_mask` bits (1 -> 0 only, as real NOR flash requires) at byte 0 of
 * the physical page - bypassing the shim to write directly to the parent,
 * the same way `ecc_test inject` does on hardware.
 */
static void write_and_corrupt(uint8_t flip_mask)
{
	uint8_t page[DATA_SIZE];

	zassert_ok(flash_erase(shim_dev, 0, SECTOR_SIZE));

	memset(page, 0xAA, sizeof(page));
	zassert_ok(flash_write(shim_dev, 0, page, sizeof(page)));

	uint8_t corrupt = 0xAA & ~flip_mask;

	zassert_ok(flash_write(parent_dev, 0, &corrupt, 1));
}

static void *suite_setup(void)
{
	zassert_true(device_is_ready(shim_dev), "shim device not ready");
	parent_dev = ecc_shim_flash_get_parent(shim_dev);
	zassert_true(device_is_ready(parent_dev), "parent device not ready");
	return NULL;
}

ZTEST_SUITE(flash_ecc_shim_stats, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(flash_ecc_shim_stats, test_1bit_corrected_increments_bit1_corrected)
{
	uint32_t before = read_stat("bit1_corrected");

	write_and_corrupt(BIT(1));

	uint8_t readback[DATA_SIZE];
	uint8_t expected[DATA_SIZE];

	zassert_ok(flash_read(shim_dev, 0, readback, sizeof(readback)));
	memset(expected, 0xAA, sizeof(expected));
	zassert_mem_equal(readback, expected, sizeof(expected));

	zassert_equal(read_stat("bit1_corrected"), before + 1);
}

ZTEST(flash_ecc_shim_stats, test_uncorrectable_increments_uncorrectable)
{
	uint32_t before = read_stat("uncorrectable");

	/* 3 bits: uncorrectable regardless of CONFIG_ECC_CRC32_2BIT_CORRECTION. */
	write_and_corrupt(BIT(1) | BIT(3) | BIT(5));

	uint8_t readback[DATA_SIZE];

	zassert_equal(flash_read(shim_dev, 0, readback, sizeof(readback)), -EFAULT);
	zassert_equal(read_stat("uncorrectable"), before + 1);
}

/* A page written through the shim and left intact must read back exactly, with no ECC event */
ZTEST(flash_ecc_shim_stats, test_clean_page_reads_back_without_ecc_event)
{
	uint8_t page[DATA_SIZE];
	uint8_t readback[DATA_SIZE];
	const uint32_t bit1_before = read_stat("bit1_corrected");
	const uint32_t uncorrectable_before = read_stat("uncorrectable");

	/* Counting pattern, so a shifted or misplaced CRC cannot go unnoticed */
	for (size_t i = 0; i < sizeof(page); i++) {
		page[i] = (uint8_t)i;
	}

	zassert_ok(flash_erase(shim_dev, 0, SECTOR_SIZE));
	zassert_ok(flash_write(shim_dev, 0, page, sizeof(page)));

	memset(readback, 0, sizeof(readback));
	zassert_ok(flash_read(shim_dev, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, page, sizeof(page), "clean page did not read back unchanged");

	zassert_equal(read_stat("bit1_corrected"), bit1_before,
		      "reading a clean page must not report a corrected error");
	zassert_equal(read_stat("uncorrectable"), uncorrectable_before,
		      "reading a clean page must not report an uncorrectable error");
}

/*
 * An erased page has no valid CRC, so the shim must return it as erase_value
 * without running the ECC check - and without touching any of the counters.
 */
ZTEST(flash_ecc_shim_stats, test_erased_page_read_as_erase_value_without_ecc_check)
{
	static uint8_t readback[SECTOR_SIZE];
	const uint8_t erase_value = flash_get_parameters(shim_dev)->erase_value;
	const uint32_t bit1_before = read_stat("bit1_corrected");
	const uint32_t uncorrectable_before = read_stat("uncorrectable");

	zassert_ok(flash_erase(shim_dev, 0, SECTOR_SIZE));

	/* Pre-fill with a value that differs from erase_value to expose a missing fill */
	memset(readback, ~erase_value, sizeof(readback));
	zassert_ok(flash_read(shim_dev, 0, readback, sizeof(readback)));

	for (size_t i = 0; i < sizeof(readback); i++) {
		zassert_equal(readback[i], erase_value,
			      "byte %zu of the erased sector is 0x%02x, expected 0x%02x", i,
			      readback[i], erase_value);
	}

	zassert_equal(read_stat("bit1_corrected"), bit1_before,
		      "reading an erased page must not report a corrected error");
	zassert_equal(read_stat("uncorrectable"), uncorrectable_before,
		      "reading an erased page must not report an uncorrectable error");
}

/* Offset/length combinations that are not whole multiples of the 252-byte data size */
static const struct {
	off_t offset;
	size_t len;
} unaligned_cases[] = {
	{1, DATA_SIZE},                /* offset off by one */
	{DATA_SIZE - 1, DATA_SIZE},    /* offset just below the next page */
	{DATA_SIZE + 1, DATA_SIZE},    /* offset just above the next page */
	{0, 1},                        /* length shorter than a page */
	{0, DATA_SIZE - 1},            /* length just below a page */
	{0, DATA_SIZE + 1},            /* length just above a page */
	{DATA_SIZE, 2 * DATA_SIZE - 1} /* aligned offset, unaligned multi-page length */
};

ZTEST(flash_ecc_shim_stats, test_unaligned_read_rejected)
{
	static uint8_t buf[2 * DATA_SIZE];
	static uint8_t expected[2 * DATA_SIZE];

	zassert_ok(flash_erase(shim_dev, 0, SECTOR_SIZE));

	for (size_t i = 0; i < ARRAY_SIZE(unaligned_cases); i++) {
		memset(buf, 0x5A, sizeof(buf));
		memset(expected, 0x5A, sizeof(expected));

		zassert_equal(flash_read(shim_dev, unaligned_cases[i].offset, buf,
					 unaligned_cases[i].len),
			      -EINVAL, "read off=%ld len=%zu not rejected with -EINVAL",
			      (long)unaligned_cases[i].offset, unaligned_cases[i].len);
		zassert_mem_equal(buf, expected, sizeof(buf),
				  "buffer modified by rejected read off=%ld len=%zu",
				  (long)unaligned_cases[i].offset, unaligned_cases[i].len);
	}

	/* A rejected read must not leave the shim locked or otherwise unusable */
	zassert_ok(flash_read(shim_dev, 0, buf, DATA_SIZE));
}

ZTEST(flash_ecc_shim_stats, test_unaligned_write_rejected)
{
	static uint8_t buf[2 * DATA_SIZE];
	static uint8_t readback[SECTOR_SIZE];
	const uint8_t erase_value = flash_get_parameters(shim_dev)->erase_value;

	zassert_ok(flash_erase(shim_dev, 0, SECTOR_SIZE));
	memset(buf, 0xAA, sizeof(buf));

	for (size_t i = 0; i < ARRAY_SIZE(unaligned_cases); i++) {
		zassert_equal(flash_write(shim_dev, unaligned_cases[i].offset, buf,
					  unaligned_cases[i].len),
			      -EINVAL, "write off=%ld len=%zu not rejected with -EINVAL",
			      (long)unaligned_cases[i].offset, unaligned_cases[i].len);
	}

	/* None of the rejected writes may have reached the flash */
	zassert_ok(flash_read(shim_dev, 0, readback, sizeof(readback)));
	for (size_t i = 0; i < sizeof(readback); i++) {
		zassert_equal(readback[i], erase_value,
			      "byte %zu changed to 0x%02x by a rejected write", i, readback[i]);
	}

	/* A rejected write must not leave the shim locked or otherwise unusable */
	zassert_ok(flash_write(shim_dev, 0, buf, DATA_SIZE));
}

#if defined(CONFIG_ECC_CRC32_2BIT_CORRECTION)
ZTEST(flash_ecc_shim_stats, test_2bit_corrected_increments_bit2_corrected)
{
	uint32_t before = read_stat("bit2_corrected");

	write_and_corrupt(BIT(1) | BIT(3));

	uint8_t readback[DATA_SIZE];
	uint8_t expected[DATA_SIZE];

	zassert_ok(flash_read(shim_dev, 0, readback, sizeof(readback)));
	memset(expected, 0xAA, sizeof(expected));
	zassert_mem_equal(readback, expected, sizeof(expected));

	zassert_equal(read_stat("bit2_corrected"), before + 1);
}
#else
ZTEST(flash_ecc_shim_stats, test_2bit_error_reported_uncorrectable_when_disabled)
{
	zassert_false(stat_exists("bit2_corrected"),
		     "bit2_corrected must not exist when CONFIG_ECC_CRC32_2BIT_CORRECTION is off");

	uint32_t before = read_stat("uncorrectable");

	write_and_corrupt(BIT(1) | BIT(3));

	uint8_t readback[DATA_SIZE];

	zassert_equal(flash_read(shim_dev, 0, readback, sizeof(readback)), -EFAULT,
		     "a 2-bit error must report uncorrectable when 2-bit correction is disabled");
	zassert_equal(read_stat("uncorrectable"), before + 1);
}
#endif /* CONFIG_ECC_CRC32_2BIT_CORRECTION */
