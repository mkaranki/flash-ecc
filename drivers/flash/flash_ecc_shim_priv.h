/*
 * Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Internal types shared between flash_ecc_shim.c and flash_ecc_shim_shell.c */
#ifndef FLASH_ECC_SHIM_PRIV_H_
#define FLASH_ECC_SHIM_PRIV_H_

#include <zephyr/drivers/flash.h>

/* CRC-32 appended to every physical page (4-byte IEEE CRC-32). */
#define ECC_CRC_SIZE 4U

struct ecc_shim_config {
	const struct device *parent;
	uint16_t data_size;        /* DT data-size:        data bytes per page */
	uint16_t pages_per_sector; /* DT pages-per-sector: physical pages per erase sector */
	struct flash_parameters flash_params;
};

struct ecc_shim_data {
	struct flash_pages_layout layout;
	/* Scratch buffer for one physical page (data_size + ECC_CRC_SIZE bytes).
	 * Kept in instance data rather than on the stack to reduce peak stack
	 * depth on callers such as the storage migration path. */
	uint8_t *page_buf;
};

#endif /* FLASH_ECC_SHIM_PRIV_H_ */
