/**
 * @file flash_ecc_shim.h
 * @brief Public API for the ECC flash shim driver.
 * @copyright Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef FLASH_ECC_SHIM_H_
#define FLASH_ECC_SHIM_H_

#include <zephyr/device.h>

/** CRC-32 bytes appended to every physical NOR page by the ECC shim. */
#define ECC_SHIM_CRC_SIZE 4U

/**
 * @brief Resolve the physical flash device backing an ECC shim device.
 *
 * The ECC shim's `struct device` has its own, much smaller, driver data
 * than the physical flash device it wraps. Passing a shim device into APIs
 * that are only meant for the physical flash driver (e.g. vendor-specific
 * extensions reached via `flash_area_get_device()`) misinterprets that
 * data and reads/writes out of bounds.
 *
 * If @p dev is an ECC flash shim instance, returns its parent (physical)
 * flash device. Otherwise returns @p dev unchanged, so callers can use
 * this unconditionally without needing to know whether a given flash
 * area sits on top of the shim.
 *
 * @param dev Flash device, possibly an ECC shim instance.
 * @return The underlying physical flash device.
 */
const struct device *ecc_shim_flash_get_parent(const struct device *dev);

#endif /* FLASH_ECC_SHIM_H_ */
