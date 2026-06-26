/*
 * ECC flash shim driver.
 *
 * Wraps a parent NOR flash device, providing transparent CRC-32 based
 * error detection and correction at the page level.
 *
 * Physical page layout (256 bytes):
 *   [byte 0..251]   252 bytes data payload
 *   [byte 252..255] CRC-32, little-endian
 *
 * Virtual geometry presented to upper layers:
 *   write_block_size = 252 bytes
 *   Virtual sector   = 4032 bytes (16 pages), maps to 4096-byte physical sector
 *
 * Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include <ecc_crc32.h>
#include "flash_ecc_shim_priv.h"

LOG_MODULE_REGISTER(ecc_flash_shim, CONFIG_FLASH_ECC_SHIM_LOG_LEVEL);

#define DT_DRV_COMPAT vaisala_ecc_flash_shim

#define ECC_DATA_SIZE        252U
#define ECC_CRC_SIZE         4U
#define ECC_PAGE_SIZE        (ECC_DATA_SIZE + ECC_CRC_SIZE) /* 256 */
#define ECC_PAGES_PER_SECTOR 16U
#define ECC_VIRT_SECTOR      (ECC_DATA_SIZE * ECC_PAGES_PER_SECTOR) /* 4032 */
#define ECC_PHYS_SECTOR      (ECC_PAGE_SIZE * ECC_PAGES_PER_SECTOR) /* 4096 */

struct ecc_shim_data {
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	struct flash_pages_layout layout;
#endif
};

static inline off_t virt_to_phys(off_t virt_off)
{
	return (virt_off / ECC_DATA_SIZE) * ECC_PAGE_SIZE;
}

static int ecc_shim_read(const struct device *dev, off_t virt_off,
			 void *buf, size_t len)
{
	const struct ecc_shim_config *cfg = dev->config;
	uint8_t page[ECC_PAGE_SIZE];
	uint8_t *dst = buf;
	int ret;

	if ((virt_off % ECC_DATA_SIZE) != 0 || (len % ECC_DATA_SIZE) != 0) {
		LOG_ERR("unaligned read off=0x%lx len=%zu (must be multiples of %u)",
			(unsigned long)virt_off, len, ECC_DATA_SIZE);
		return -EINVAL;
	}

	while (len > 0) {
		off_t phys_off = virt_to_phys(virt_off);

		ret = flash_read(cfg->parent, phys_off, page, ECC_PAGE_SIZE);
		if (ret != 0) {
			return ret;
		}

		/* Erased page: all bytes 0xFF, return as-is without ECC check */
		bool erased = true;

		for (size_t i = 0; i < ECC_PAGE_SIZE; i++) {
			if (page[i] != 0xFF) {
				erased = false;
				break;
			}
		}

		if (erased) {
			memset(dst, 0xFF, ECC_DATA_SIZE);
		} else {
			uint32_t stored_crc = sys_get_le32(&page[ECC_DATA_SIZE]);
			int ecc = ecc_crc32_correct(page, ECC_DATA_SIZE, stored_crc);

			uint32_t block = (uint32_t)(phys_off / ECC_PHYS_SECTOR);
			uint32_t page_in_block =
				(uint32_t)((phys_off % ECC_PHYS_SECTOR) / ECC_PAGE_SIZE);

			switch (ecc) {
			case ECC_CRC32_OK:
				break;
			case ECC_CRC32_CORRECTED_1BIT:
				LOG_WRN("1-bit error corrected at 0x%x (block %u page %u)",
					(uint32_t)phys_off, block, page_in_block);
				break;
			case ECC_CRC32_CORRECTED_2BIT:
				LOG_ERR("2-bit error corrected at 0x%x (block %u page %u)",
					(uint32_t)phys_off, block, page_in_block);
				break;
			default: /* ECC_CRC32_UNCORRECTABLE */
				LOG_ERR("Uncorrectable error at 0x%x (block %u page %u)",
					(uint32_t)phys_off, block, page_in_block);
				/* -EFAULT maps to LFS_ERR_CORRUPT, which LittleFS
				 * treats as "data is garbage, block can be reused".
				 * -EIO would map to LFS_ERR_IO and abort operations
				 * like format that deliberately read uninitialized
				 * blocks to retrieve the old revision counter. */
				return -EFAULT;
			}

			memcpy(dst, page, ECC_DATA_SIZE);
		}

		dst += ECC_DATA_SIZE;
		virt_off += ECC_DATA_SIZE;
		len -= ECC_DATA_SIZE;
	}

	return 0;
}

static int ecc_shim_write(const struct device *dev, off_t virt_off,
			  const void *buf, size_t len)
{
	const struct ecc_shim_config *cfg = dev->config;
	uint8_t page[ECC_PAGE_SIZE];
	const uint8_t *src = buf;
	int ret;

	if ((virt_off % ECC_DATA_SIZE) != 0 || (len % ECC_DATA_SIZE) != 0) {
		LOG_ERR("unaligned write off=0x%lx len=%zu (must be multiples of %u)",
			(unsigned long)virt_off, len, ECC_DATA_SIZE);
		return -EINVAL;
	}

	while (len > 0) {
		off_t phys_off = virt_to_phys(virt_off);

		memcpy(page, src, ECC_DATA_SIZE);
		uint32_t crc = ecc_crc32_encode(page, ECC_DATA_SIZE);
		sys_put_le32(crc, &page[ECC_DATA_SIZE]);

		ret = flash_write(cfg->parent, phys_off, page, ECC_PAGE_SIZE);
		if (ret != 0) {
			return ret;
		}

		src += ECC_DATA_SIZE;
		virt_off += ECC_DATA_SIZE;
		len -= ECC_DATA_SIZE;
	}

	return 0;
}

static int ecc_shim_erase(const struct device *dev, off_t virt_off, size_t virt_len)
{
	const struct ecc_shim_config *cfg = dev->config;
	off_t phys_off = (virt_off / ECC_VIRT_SECTOR) * ECC_PHYS_SECTOR;
	size_t phys_len = (virt_len / ECC_VIRT_SECTOR) * ECC_PHYS_SECTOR;

	return flash_erase(cfg->parent, phys_off, phys_len);
}

static const struct flash_parameters ecc_shim_params = {
	.write_block_size = ECC_DATA_SIZE,
	.erase_value = 0xFF,
};

static const struct flash_parameters *ecc_shim_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);
	return &ecc_shim_params;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)

static bool count_phys_sectors_cb(const struct flash_pages_info *info, void *data)
{
	size_t *count = data;

	*count += info->size / ECC_PHYS_SECTOR;
	return true;
}

static void ecc_shim_page_layout(const struct device *dev,
				 const struct flash_pages_layout **layout,
				 size_t *layout_size)
{
	struct ecc_shim_data *data = dev->data;

	*layout = &data->layout;
	*layout_size = 1;
}

#endif /* CONFIG_FLASH_PAGE_LAYOUT */

static int ecc_shim_init(const struct device *dev)
{
	const struct ecc_shim_config *cfg = dev->config;

	if (!device_is_ready(cfg->parent)) {
		LOG_ERR("Parent flash device not ready");
		return -ENODEV;
	}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	struct ecc_shim_data *data = dev->data;
	size_t sector_count = 0;

	flash_page_foreach(cfg->parent, count_phys_sectors_cb, &sector_count);

	data->layout.pages_size = ECC_VIRT_SECTOR;
	data->layout.pages_count = sector_count;
#endif

	return 0;
}

static const struct flash_driver_api ecc_shim_api = {
	.read = ecc_shim_read,
	.write = ecc_shim_write,
	.erase = ecc_shim_erase,
	.get_parameters = ecc_shim_get_parameters,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = ecc_shim_page_layout,
#endif
};

#define ECC_SHIM_DEFINE(inst)                                                   \
	static struct ecc_shim_data ecc_shim_data_##inst;                       \
                                                                                \
	static const struct ecc_shim_config ecc_shim_cfg_##inst = {             \
		.parent = DEVICE_DT_GET(DT_INST_PHANDLE(inst, parent_flash)),   \
	};                                                                      \
                                                                                \
	DEVICE_DT_INST_DEFINE(inst,                                             \
			      ecc_shim_init,                                    \
			      NULL,                                             \
			      &ecc_shim_data_##inst,                            \
			      &ecc_shim_cfg_##inst,                             \
			      POST_KERNEL,                                      \
			      CONFIG_FLASH_INIT_PRIORITY,                       \
			      &ecc_shim_api);

DT_INST_FOREACH_STATUS_OKAY(ECC_SHIM_DEFINE)
