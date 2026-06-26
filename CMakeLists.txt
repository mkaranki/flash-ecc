# Copyright (c) Vaisala Oyj.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.20.0)

zephyr_include_directories(include)
add_subdirectory_ifdef(CONFIG_ECC_CRC32 lib/ecc_crc32)
add_subdirectory_ifdef(CONFIG_FLASH_ECC_SHIM drivers/flash)
