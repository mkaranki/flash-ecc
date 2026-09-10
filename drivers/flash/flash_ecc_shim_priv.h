/*
 * Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Internal types shared between flash_ecc_shim.c and flash_ecc_shim_shell.c */
#ifndef FLASH_ECC_SHIM_PRIV_H_
#define FLASH_ECC_SHIM_PRIV_H_

#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <flash_ecc_shim.h> /* ECC_SHIM_CRC_SIZE */

struct ecc_shim_config {
	const struct device *parent;
	uint16_t data_size;        /* DT data-size:        data bytes per page */
	uint16_t pages_per_sector; /* DT pages-per-sector: physical pages per erase sector */
	struct flash_parameters flash_params;
};

struct ecc_shim_data {
	struct flash_pages_layout layout;
	/* Scratch buffer for one physical page (data_size + ECC_SHIM_CRC_SIZE bytes).
	 * Kept in instance data rather than on the stack to reduce peak stack
	 * depth on callers such as the storage migration path.
	 *
	 * This buffer is shared by every read/write/erase call on the device,
	 * so it must never be accessed by two callers at once - see `lock`. */
	uint8_t *page_buf;
	/* Serializes all shim entry points against `page_buf` above. Nothing
	 * upstream of this driver can be relied on to do that for us: LittleFS's
	 * own per-mount mutex only serializes callers of *that* mount, and two
	 * different mounts (e.g. the primary filesystem and the one-time
	 * migration staging area) can be layered on the same underlying shim
	 * device without ever sharing a mutex. The parent flash driver's own
	 * locking (e.g. flash_stm32_ospi's per-device semaphore) only covers the
	 * physical bus transaction, not this driver's buffer handling around it. */
	struct k_mutex lock;
};

#endif /* FLASH_ECC_SHIM_PRIV_H_ */
