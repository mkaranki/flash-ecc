/*
 * CRC-32 based error detection and correction.
 *
 * Correction algorithm adapted from:
 *   https://github.com/littlefs-project/ramcrc32bd
 *   Copyright (c) 2024, The littlefs authors.
 *   SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/sys/crc.h>
#include <ecc_crc32.h>

/* CRC-32 reflected polynomial (IEEE 802.3). Normal form: 0x04C11DB7. */
#define CRC32_POLY_REFLECTED 0xEDB88320U

/*
 * Standard IEEE CRC-32 (reflected polynomial CRC32_POLY_REFLECTED,
 * init=0xFFFFFFFF, final XOR=0xFFFFFFFF).  Zephyr's crc32_ieee() implements
 * this directly.
 *
 * Note: this differs from LittleFS's internal CRC (init=0, no final XOR).
 * That is fine — the ECC CRC protects physical pages at a layer below
 * LittleFS's own checksums; the two never interact.
 *
 * The correction algorithm in ecc_crc32_correct() is init-independent:
 * syndrome = crc(corrupted) ^ crc(original), and the init contributions
 * cancel in that XOR regardless of the chosen init value.
 */
uint32_t ecc_crc32_encode(const uint8_t *data, size_t size)
{
	return crc32_ieee(data, size);
}

int ecc_crc32_correct(uint8_t *data, size_t size, uint32_t stored_crc)
{
	uint32_t computed = ecc_crc32_encode(data, size);

	if (computed == stored_crc) {
		return ECC_CRC32_OK;
	}

	uint32_t syndrome = computed ^ stored_crc;
	size_t codeword_bits = (size + sizeof(uint32_t)) * 8;

	/*
	 * Try to correct a 1-bit error.
	 *
	 * For each bit position in the codeword (data + CRC), compute the
	 * CRC contribution 'e' of flipping that bit using the CRC LFSR shift
	 * relation. If e matches the syndrome, the error is at that position.
	 *
	 * Adapted from geky/ramcrc32bd (BSD-3-Clause).
	 */
	uint32_t e = 0x80000000;

	for (size_t i = 0; i < codeword_bits; i++) {
		if (e == syndrome) {
			size_t bit = codeword_bits - 1 - i;

			if (bit / 8 < size) {
				data[bit / 8] ^= 1u << (bit % 8);
			}
			return ECC_CRC32_CORRECTED_1BIT;
		}
		e = (e >> 1) ^ ((e & 1) ? CRC32_POLY_REFLECTED : 0);
	}

#if defined(CONFIG_ECC_CRC32_2BIT_CORRECTION)
	/*
	 * Try to correct a 2-bit error.
	 *
	 * Check all pairs of bit positions. Both e0 and e1 are derived
	 * incrementally via the LFSR; no CRC recomputation in the inner loop.
	 *
	 * WARNING: O(n²) where n = codeword_bits = 2048 → ~250 ms per page
	 * on a Cortex-M33 at 160 MHz.  See CONFIG_ECC_CRC32_2BIT_CORRECTION.
	 *
	 * Adapted from geky/ramcrc32bd (BSD-3-Clause).
	 */
	uint32_t e0 = 0x80000000;

	for (size_t i0 = 0; i0 < codeword_bits; i0++) {
		uint32_t e1 = 0x80000000;

		for (size_t i1 = 0; i1 < codeword_bits; i1++) {
			if ((e0 ^ e1) == syndrome) {
				size_t bit0 = codeword_bits - 1 - i0;
				size_t bit1 = codeword_bits - 1 - i1;

				if (bit0 / 8 < size) {
					data[bit0 / 8] ^= 1u << (bit0 % 8);
				}
				if (bit1 / 8 < size) {
					data[bit1 / 8] ^= 1u << (bit1 % 8);
				}
				return ECC_CRC32_CORRECTED_2BIT;
			}
			e1 = (e1 >> 1) ^ ((e1 & 1) ? CRC32_POLY_REFLECTED : 0);
		}
		e0 = (e0 >> 1) ^ ((e0 & 1) ? CRC32_POLY_REFLECTED : 0);
	}
#endif /* CONFIG_ECC_CRC32_2BIT_CORRECTION */

	return ECC_CRC32_UNCORRECTABLE;
}
