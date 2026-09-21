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
