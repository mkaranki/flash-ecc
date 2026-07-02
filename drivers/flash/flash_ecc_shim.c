/*
 * ECC flash shim driver.
 *
 * Wraps a parent NOR flash device, providing transparent CRC-32 based
 * error detection and correction at the page level.
 *
 * Physical page layout (default: 256 bytes):
 *   [byte 0..N-1]   N bytes data payload  (DT data-size, default 252)
 *   [byte N..N+3]   CRC-32, little-endian (ECC_CRC_SIZE = 4)
 *
 * Virtual geometry presented to upper layers (default values):
 *   write_block_size = 252 bytes  (data-size)
 *   Virtual sector   = 4032 bytes (data-size × pages-per-sector)
 *   maps to 4096-byte physical sector (page-size × pages-per-sector)
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

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct ecc_shim_data {
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	struct flash_pages_layout layout;
#endif
};

static inline off_t virt_to_phys(const struct ecc_shim_config *cfg, off_t virt_off)
{
	return (virt_off / cfg->data_size) * (cfg->data_size + ECC_CRC_SIZE);
}

static int ecc_shim_read(const struct device *dev, off_t virt_off,
			 void *buf, size_t len)
{
	const struct ecc_shim_config *cfg = dev->config;
	const uint16_t data_size = cfg->data_size;
	const uint16_t page_size = data_size + ECC_CRC_SIZE;
	const uint32_t phys_sector = (uint32_t)page_size * cfg->pages_per_sector;
	uint8_t page[page_size];
	uint8_t *dst = buf;
	int ret;

	if ((virt_off % data_size) != 0 || (len % data_size) != 0) {
		LOG_ERR("unaligned read off=0x%lx len=%zu (must be multiples of %u)",
			(unsigned long)virt_off, len, data_size);
		return -EINVAL;
	}

	while (len > 0) {
		off_t phys_off = virt_to_phys(cfg, virt_off);

		ret = flash_read(cfg->parent, phys_off, page, page_size);
		if (ret != 0) {
			return ret;
		}

		/* Erased page: all bytes 0xFF, return as-is without ECC check */
		bool erased = true;

		for (size_t i = 0; i < page_size; i++) {
			if (page[i] != 0xFF) {
				erased = false;
				break;
			}
		}

		if (erased) {
			memset(dst, 0xFF, data_size);
		} else {
			uint32_t stored_crc = sys_get_le32(&page[data_size]);
			int ecc = ecc_crc32_correct(page, data_size, stored_crc);

			uint32_t block = (uint32_t)(phys_off / phys_sector);
			uint32_t page_in_block =
				(uint32_t)((phys_off % phys_sector) / page_size);

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

			memcpy(dst, page, data_size);
		}

		dst += data_size;
		virt_off += data_size;
		len -= data_size;
	}

	return 0;
}

static int ecc_shim_write(const struct device *dev, off_t virt_off,
			  const void *buf, size_t len)
{
	const struct ecc_shim_config *cfg = dev->config;
	const uint16_t data_size = cfg->data_size;
	const uint16_t page_size = data_size + ECC_CRC_SIZE;
	uint8_t page[page_size];
	const uint8_t *src = buf;
	int ret;

	if ((virt_off % data_size) != 0 || (len % data_size) != 0) {
		LOG_ERR("unaligned write off=0x%lx len=%zu (must be multiples of %u)",
			(unsigned long)virt_off, len, data_size);
		return -EINVAL;
	}

	while (len > 0) {
		off_t phys_off = virt_to_phys(cfg, virt_off);

		memcpy(page, src, data_size);
		uint32_t crc = ecc_crc32_encode(page, data_size);
		sys_put_le32(crc, &page[data_size]);

		ret = flash_write(cfg->parent, phys_off, page, page_size);
		if (ret != 0) {
			return ret;
		}

		src += data_size;
		virt_off += data_size;
		len -= data_size;
	}

	return 0;
}

static int ecc_shim_erase(const struct device *dev, off_t virt_off, size_t virt_len)
{
	const struct ecc_shim_config *cfg = dev->config;
	const uint32_t virt_sector = (uint32_t)cfg->data_size * cfg->pages_per_sector;
	const uint32_t phys_sector =
		(uint32_t)(cfg->data_size + ECC_CRC_SIZE) * cfg->pages_per_sector;
	const off_t phys_off = (virt_off / virt_sector) * phys_sector;
	const size_t phys_len = (virt_len / virt_sector) * phys_sector;

	return flash_erase(cfg->parent, phys_off, phys_len);
}

static const struct flash_parameters *ecc_shim_get_parameters(const struct device *dev)
{
	const struct ecc_shim_config *cfg = dev->config;

	return &cfg->flash_params;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)

struct count_cb_data {
	size_t count;
	uint32_t phys_sector;
};

static bool count_phys_sectors_cb(const struct flash_pages_info *info, void *data)
{
	struct count_cb_data *d = data;

	d->count += info->size / d->phys_sector;
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
	struct count_cb_data cb = {
		.count = 0,
		.phys_sector = (uint32_t)(cfg->data_size + ECC_CRC_SIZE) * cfg->pages_per_sector,
	};

	flash_page_foreach(cfg->parent, count_phys_sectors_cb, &cb);

	data->layout.pages_size = (uint32_t)cfg->data_size * cfg->pages_per_sector;
	data->layout.pages_count = cb.count;
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
		.data_size = DT_INST_PROP(inst, data_size),                     \
		.pages_per_sector = DT_INST_PROP(inst, pages_per_sector),       \
		.flash_params = {                                               \
			.write_block_size = DT_INST_PROP(inst, data_size),      \
			.erase_value = 0xFF,                                    \
		},                                                              \
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

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
