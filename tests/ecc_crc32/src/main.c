/*
 * Unit tests for the ecc_crc32 library.
 *
 * Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/ztest.h>
#include <ecc_crc32.h>
#include <inttypes.h>
#include <string.h>

#define DATA_SIZE 252U

ZTEST_SUITE(ecc_crc32, NULL, NULL, NULL, NULL, NULL);

/*
 * Flip data bit(s) data_mask in data[byte] (none if data_mask is 0) and the
 * stored CRC bits in crc_mask, then check that ecc_crc32_correct() reports
 * "expected" and leaves the data identical to the original.
 */
static void check_flips(size_t byte, uint8_t data_mask, uint32_t crc_mask, int expected)
{
	uint8_t orig[DATA_SIZE], data[DATA_SIZE];

	memset(orig, 0xAB, DATA_SIZE);
	const uint32_t crc = ecc_crc32_encode(orig, DATA_SIZE);

	memcpy(data, orig, DATA_SIZE);
	data[byte] ^= data_mask;

	const int ret = ecc_crc32_correct(data, DATA_SIZE, crc ^ crc_mask);

	zassert_equal(ret, expected,
		      "data byte %zu mask 0x%02" PRIx8 " + stored CRC mask 0x%08" PRIx32
		      ": got %d, expected %d",
		      byte, data_mask, crc_mask, ret, expected);
	zassert_mem_equal(data, orig, DATA_SIZE,
			  "data byte %zu mask 0x%02" PRIx8 " not restored or wrongly modified "
			  "(stored CRC mask 0x%08" PRIx32 ")",
			  byte, data_mask, crc_mask);
}

ZTEST(ecc_crc32, test_no_error)
{
	uint8_t data[DATA_SIZE];

	memset(data, 0xAB, DATA_SIZE);
	const uint32_t crc = ecc_crc32_encode(data, DATA_SIZE);

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_OK);
}

ZTEST(ecc_crc32, test_no_error_all_zeros)
{
	uint8_t data[DATA_SIZE];

	memset(data, 0x00, DATA_SIZE);
	const uint32_t crc = ecc_crc32_encode(data, DATA_SIZE);

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_OK);
}

ZTEST(ecc_crc32, test_1bit_correction_first_byte)
{
	check_flips(0, BIT(0), 0, ECC_CRC32_CORRECTED_1BIT);
}

ZTEST(ecc_crc32, test_1bit_correction_last_byte)
{
	check_flips(DATA_SIZE - 1, BIT(7), 0, ECC_CRC32_CORRECTED_1BIT);
}

ZTEST(ecc_crc32, test_1bit_correction_mid_byte)
{
	check_flips(100, BIT(3), 0, ECC_CRC32_CORRECTED_1BIT);
}

ZTEST(ecc_crc32, test_2bit_correction_adjacent)
{
	check_flips(0, BIT(0) | BIT(1), 0, ECC_CRC32_CORRECTED_2BIT);
}

ZTEST(ecc_crc32, test_2bit_correction_spread)
{
	uint8_t orig[DATA_SIZE], data[DATA_SIZE];

	memset(orig, 0xAB, DATA_SIZE);
	const uint32_t crc = ecc_crc32_encode(orig, DATA_SIZE);

	memcpy(data, orig, DATA_SIZE);
	data[0] ^= BIT(0);
	data[200] ^= BIT(7);

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_CORRECTED_2BIT);
	zassert_mem_equal(data, orig, DATA_SIZE);
}

ZTEST(ecc_crc32, test_uncorrectable_3bit)
{
	uint8_t data[DATA_SIZE];

	memset(data, 0xAB, DATA_SIZE);
	const uint32_t crc = ecc_crc32_encode(data, DATA_SIZE);

	data[0] ^= BIT(0) | BIT(1) | BIT(2); /* three bits in one byte */

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_UNCORRECTABLE);
}

ZTEST(ecc_crc32, test_1bit_crc_flip_data_untouched)
{
	/* every single bit of the stored CRC must be recognized as the error */
	for (unsigned int bit = 0; bit < 32; bit++) {
		check_flips(0, 0, BIT(bit), ECC_CRC32_CORRECTED_1BIT);
	}
}

ZTEST(ecc_crc32, test_2bit_crc_flip_data_untouched)
{
	/* CRC bit pairs: extremes, adjacent at each end, and a mid pair */
	const uint32_t crc_flips[] = {
		BIT(31) | BIT(0),
		BIT(1) | BIT(0),
		BIT(31) | BIT(30),
		BIT(16) | BIT(8),
	};

	for (size_t i = 0; i < ARRAY_SIZE(crc_flips); i++) {
		check_flips(0, 0, crc_flips[i], ECC_CRC32_CORRECTED_2BIT);
	}
}

ZTEST(ecc_crc32, test_2bit_data_and_crc_flip)
{
	/* one flipped data bit plus one flipped CRC bit: data must be repaired */
	const struct {
		size_t byte;
		uint8_t mask;
		unsigned int crc_bit;
	} cases[] = {
		{0, BIT(0), 31},            /* first data bit + last CRC bit: loop extremes */
		{DATA_SIZE - 1, BIT(7), 0}, /* last data bit + first CRC bit: data/CRC boundary */
		{100, BIT(4), 0},
		{100, BIT(4), 7},
		{100, BIT(4), 14},
		{100, BIT(4), 21},
		{100, BIT(4), 28},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		check_flips(cases[i].byte, cases[i].mask, BIT(cases[i].crc_bit),
			    ECC_CRC32_CORRECTED_2BIT);
	}
}
