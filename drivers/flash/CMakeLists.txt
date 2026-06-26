# Copyright (c) Vaisala Oyj.
# SPDX-License-Identifier: BSD-3-Clause

zephyr_library()
zephyr_library_sources(flash_ecc_shim.c)
zephyr_library_sources_ifdef(CONFIG_FLASH_ECC_SHIM_SHELL flash_ecc_shim_shell.c)
