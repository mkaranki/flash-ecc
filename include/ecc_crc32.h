/*
 * CRC-32 based error detection and correction.
 *
 * Copyright (c) 2024, The littlefs authors (algorithm basis).
 * Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** No error detected. */
#define ECC_CRC32_OK             0
/** Single-bit error detected and corrected. */
#define ECC_CRC32_CORRECTED_1BIT 1
/** Two-bit error detected and corrected. */
#define ECC_CRC32_CORRECTED_2BIT 2
/** Error detected, correction not possible (more than 2 bits). */
#define ECC_CRC32_UNCORRECTABLE  -1

/**
 * @brief Compute CRC-32 checksum for a data buffer.
 *
 * Uses standard IEEE CRC-32: reflected polynomial 0xEDB88320 (= 0x04C11DB7),
 * init 0xFFFFFFFF, final XOR 0xFFFFFFFF. Implemented via Zephyr's crc32_ieee().
 *
 * @param data  Input data
 * @param size  Number of bytes
 * @return CRC-32 value
 */
uint32_t ecc_crc32_encode(const uint8_t *data, size_t size);

/**
 * @brief Validate and correct a data buffer using a stored CRC-32.
 *
 * Always corrects 1-bit errors. 2-bit correction requires
 * CONFIG_ECC_CRC32_2BIT_CORRECTION=y; without it, 2-bit errors are detected
 * but returned as ECC_CRC32_UNCORRECTABLE. Corrected data is written back
 * in-place.
 *
 * The effective codeword is @p size + 4 bytes (data + CRC). For reliable
 * 2-bit correction the codeword must not exceed 371 bytes, meaning
 * @p size <= 367 bytes.
 *
 * @param data        Data buffer, corrected in-place on ECC_CRC32_CORRECTED_*
 * @param size        Number of data bytes (not including CRC)
 * @param stored_crc  CRC-32 stored alongside the data
 *
 * @retval ECC_CRC32_OK             Data is valid, no correction needed.
 * @retval ECC_CRC32_CORRECTED_1BIT Single-bit error corrected.
 * @retval ECC_CRC32_CORRECTED_2BIT Two-bit error corrected (requires CONFIG_ECC_CRC32_2BIT_CORRECTION=y).
 * @retval ECC_CRC32_UNCORRECTABLE  Uncorrectable error (>2 bits).
 */
int ecc_crc32_correct(uint8_t *data, size_t size, uint32_t stored_crc);

#ifdef __cplusplus
}
#endif
