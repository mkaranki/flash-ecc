/*
 * Unit tests for the ecc_crc32 library.
 *
 * Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/ztest.h>
#include <ecc_crc32.h>
#include <string.h>

#define DATA_SIZE 252U

ZTEST_SUITE(ecc_crc32, NULL, NULL, NULL, NULL, NULL);

ZTEST(ecc_crc32, test_no_error)
{
	uint8_t data[DATA_SIZE];

	memset(data, 0xAB, DATA_SIZE);
	uint32_t crc = ecc_crc32_encode(data, DATA_SIZE);

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_OK);
}

ZTEST(ecc_crc32, test_no_error_all_zeros)
{
	uint8_t data[DATA_SIZE];

	memset(data, 0x00, DATA_SIZE);
	uint32_t crc = ecc_crc32_encode(data, DATA_SIZE);

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_OK);
}

ZTEST(ecc_crc32, test_1bit_correction_first_byte)
{
	uint8_t orig[DATA_SIZE], data[DATA_SIZE];

	memset(orig, 0xAB, DATA_SIZE);
	uint32_t crc = ecc_crc32_encode(orig, DATA_SIZE);

	memcpy(data, orig, DATA_SIZE);
	data[0] ^= 0x01;

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_CORRECTED_1BIT);
	zassert_mem_equal(data, orig, DATA_SIZE);
}

ZTEST(ecc_crc32, test_1bit_correction_last_byte)
{
	uint8_t orig[DATA_SIZE], data[DATA_SIZE];

	memset(orig, 0x55, DATA_SIZE);
	uint32_t crc = ecc_crc32_encode(orig, DATA_SIZE);

	memcpy(data, orig, DATA_SIZE);
	data[DATA_SIZE - 1] ^= 0x80;

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_CORRECTED_1BIT);
	zassert_mem_equal(data, orig, DATA_SIZE);
}

ZTEST(ecc_crc32, test_1bit_correction_mid_byte)
{
	uint8_t orig[DATA_SIZE], data[DATA_SIZE];

	memset(orig, 0xFF, DATA_SIZE);
	orig[100] = 0x00;
	uint32_t crc = ecc_crc32_encode(orig, DATA_SIZE);

	memcpy(data, orig, DATA_SIZE);
	data[100] ^= 0x08;

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_CORRECTED_1BIT);
	zassert_mem_equal(data, orig, DATA_SIZE);
}

ZTEST(ecc_crc32, test_2bit_correction_adjacent)
{
	uint8_t orig[DATA_SIZE], data[DATA_SIZE];

	memset(orig, 0xAB, DATA_SIZE);
	uint32_t crc = ecc_crc32_encode(orig, DATA_SIZE);

	memcpy(data, orig, DATA_SIZE);
	data[0] ^= 0x01;
	data[0] ^= 0x02;

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_CORRECTED_2BIT);
	zassert_mem_equal(data, orig, DATA_SIZE);
}

ZTEST(ecc_crc32, test_2bit_correction_spread)
{
	uint8_t orig[DATA_SIZE], data[DATA_SIZE];

	memset(orig, 0xAB, DATA_SIZE);
	uint32_t crc = ecc_crc32_encode(orig, DATA_SIZE);

	memcpy(data, orig, DATA_SIZE);
	data[0]   ^= 0x01;
	data[200] ^= 0x80;

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_CORRECTED_2BIT);
	zassert_mem_equal(data, orig, DATA_SIZE);
}

ZTEST(ecc_crc32, test_uncorrectable_3bit)
{
	uint8_t data[DATA_SIZE];

	memset(data, 0xAB, DATA_SIZE);
	uint32_t crc = ecc_crc32_encode(data, DATA_SIZE);

	data[0] ^= 0x07; /* three bits in one byte */

	zassert_equal(ecc_crc32_correct(data, DATA_SIZE, crc), ECC_CRC32_UNCORRECTABLE);
}

ZTEST(ecc_crc32, test_wrong_crc_no_data_error)
{
	uint8_t data[DATA_SIZE];

	memset(data, 0xAB, DATA_SIZE);
	uint32_t crc = ecc_crc32_encode(data, DATA_SIZE);

	/* corrupt the stored CRC itself (simulates a CRC byte flip) */
	crc ^= 0x00000001;

	/* single bit in crc field is correctable */
	int ret = ecc_crc32_correct(data, DATA_SIZE, crc);

	zassert_true(ret == ECC_CRC32_CORRECTED_1BIT || ret == ECC_CRC32_UNCORRECTABLE,
		     "Unexpected return value %d", ret);
}
