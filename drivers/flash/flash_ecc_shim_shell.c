/*
 * ECC flash shim shell test commands.
 *
 * Provides 'ecc_test inject <path>' to write a file, locate it in physical
 * flash, corrupt one bit, and leave the file intact for post-reboot
 * verification.
 *
 * A random 8-byte nonce is embedded in the test pattern so stale copies from
 * previous runs cannot be matched — no GC call is needed.
 *
 * Copyright (c) Vaisala Oyj.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "flash_ecc_shim_priv.h"

LOG_MODULE_DECLARE(ecc_flash_shim, CONFIG_FLASH_ECC_SHIM_LOG_LEVEL);

#define DATA_SIZE    252U  /* ECC_DATA_SIZE — one virtual page */
#define PAGE_SIZE    256U  /* ECC_PAGE_SIZE — one physical page */
#define NONCE_SIZE     8U  /* random bytes at start of pattern */
#define FLIP_OFF       8U  /* offset of 0xAA byte to corrupt (past nonce) */
/* LittleFS inlines files up to min(cache_size, attr_max) bytes.
 * With cache_size=252 and Zephyr's default attr_max=1022 that threshold is
 * 252.  A 252-byte file is stored inside the directory metadata commit blob
 * rather than in a data block, so a raw page scan cannot find it.
 * Writing DATA_SIZE+1 bytes forces allocation of a real data block. */
#define WRITE_SIZE   (DATA_SIZE + 1U)

/*
 * Write a test pattern into a file, find it in physical flash, and corrupt
 * one bit. The file is left on disk so a subsequent reboot + read triggers
 * the ECC correction log message.
 *
 * Usage: ecc_test inject <filepath>
 * Example: ecc_test inject /lfs1/_ecc_test
 */
static int cmd_ecc_inject(const struct shell *sh, size_t argc, char **argv)
{
	const char *path = argv[1];

	/* Build a unique write buffer: random 8-byte nonce, rest 0xAA.
	 * Size is WRITE_SIZE (DATA_SIZE+1) to exceed the LittleFS inline
	 * threshold so the data is stored in a dedicated flash block, not
	 * embedded in directory metadata.
	 * 0xAA = 10101010 — every bit available for a 1->0 NOR-flash flip. */
	uint8_t wbuf[WRITE_SIZE];
	memset(wbuf, 0xAA, sizeof(wbuf));
	sys_put_le32(sys_rand32_get(), &wbuf[0]);
	sys_put_le32(sys_rand32_get(), &wbuf[4]);

	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc < 0) {
		shell_error(sh, "cannot open %s: %d", path, rc);
		return rc;
	}
	rc = fs_write(&f, wbuf, sizeof(wbuf));
	fs_close(&f);
	if (rc != (int)sizeof(wbuf)) {
		shell_error(sh, "write failed: %d", rc);
		return rc < 0 ? rc : -EIO;
	}

	/* The scan searches for only the first DATA_SIZE bytes (nonce + 0xAA).
	 * The extra byte forces a data block; the scan still hits page boundaries
	 * because block_size=4032 = 16*252 is always a multiple of DATA_SIZE. */
	uint8_t pattern[DATA_SIZE];
	memcpy(pattern, wbuf, DATA_SIZE);

	/* Scan the storage partition via the ECC layer, looking for our nonce.
	 * flash_area_read goes through the ECC shim so any pre-existing
	 * correctable errors on other pages would also be reported here. */
	const struct flash_area *fa;
	rc = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
	if (rc < 0) {
		shell_error(sh, "flash_area_open failed: %d", rc);
		return rc;
	}

	/* Scan physical pages directly on the parent NOR flash, bypassing the
	 * ECC shim.  Reading through the shim would trigger CRC checks and log
	 * error messages for every page whose stored CRC doesn't match (e.g.
	 * pages written before a CRC-algorithm change, or freed-but-not-erased
	 * blocks).  The scan only needs raw bytes; correctness of the CRC is
	 * irrelevant here. */
	const struct ecc_shim_config *cfg = flash_area_get_device(fa)->config;

	uint8_t raw_page[PAGE_SIZE];
	off_t found_off = -1;

	for (off_t off = 0; off < (off_t)fa->fa_size; off += DATA_SIZE) {
		off_t phys_scan = ((fa->fa_off + off) / DATA_SIZE) * PAGE_SIZE;

		rc = flash_read(cfg->parent, phys_scan, raw_page, PAGE_SIZE);
		if (rc < 0) {
			continue;
		}
		if (memcmp(raw_page, pattern, DATA_SIZE) == 0) {
			found_off = off;
			break;
		}
	}

	if (found_off < 0) {
		shell_error(sh, "pattern not found — fs may not have flushed");
		flash_area_close(fa);
		return -ENOENT;
	}

	/* Virtual address on the ECC device (partition-relative -> absolute). */
	off_t virt = fa->fa_off + found_off;
	/* Physical address on the parent NOR flash. */
	off_t phys = (virt / DATA_SIZE) * PAGE_SIZE;

	shell_print(sh, "found: partition+0x%05lx  virt=0x%06lx  phys=0x%07lx",
		    (long)found_off, (long)virt, (long)phys);

	/* Flip bit 1 of the 0xAA byte at FLIP_OFF: 0xAA (10101010) -> 0xA8 (10101000).
	 * NOR flash only supports 1->0 transitions without erasing. */
	uint8_t corrupt = pattern[FLIP_OFF] & ~BIT(1);

	rc = flash_write(cfg->parent, phys + FLIP_OFF, &corrupt, 1);
	flash_area_close(fa);
	if (rc < 0) {
		shell_error(sh, "flash_write failed: %d", rc);
		return rc;
	}

	shell_print(sh, "injected: phys+%u  0x%02x -> 0x%02x",
		    (unsigned)FLIP_OFF, pattern[FLIP_OFF], corrupt);
	shell_print(sh, "reboot then read %s — expect '[wrn] ecc_flash_shim: "
		    "1-bit error corrected'", path);
	return 0;
}

/*
 * Read the test file back through LittleFS and verify the data is intact.
 * Run this after reboot to confirm ECC correction fired.
 *
 * Usage: ecc_test verify <filepath>
 */
static int cmd_ecc_verify(const struct shell *sh, size_t argc, char **argv)
{
	const char *path = argv[1];

	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, path, FS_O_READ);
	if (rc < 0) {
		shell_error(sh, "cannot open %s: %d", path, rc);
		return rc;
	}

	uint8_t buf[WRITE_SIZE];
	rc = fs_read(&f, buf, sizeof(buf));
	fs_close(&f);
	if (rc < 0) {
		shell_error(sh, "read failed: %d", rc);
		return rc;
	}

	/* Bytes past the nonce must all be 0xAA (the extra byte is also 0xAA). */
	bool ok = true;
	for (size_t i = NONCE_SIZE; i < sizeof(buf); i++) {
		if (buf[i] != 0xAA) {
			ok = false;
			break;
		}
	}

	if (ok) {
		shell_print(sh, "PASS — data intact (check log above for correction message)");
	} else {
		shell_error(sh, "FAIL — data corrupted, ECC did not recover");
	}
	return ok ? 0 : -EIO;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ecc_test,
	SHELL_CMD_ARG(inject, NULL,
		"Inject 1-bit error: inject <filepath>\n"
		"  Writes a test file, finds it in physical flash, flips one bit.\n"
		"  File is left intact — reboot and run 'verify' to confirm correction.",
		cmd_ecc_inject, 2, 0),
	SHELL_CMD_ARG(verify, NULL,
		"Verify ECC recovery: verify <filepath>\n"
		"  Reads file through LittleFS and checks data integrity.\n"
		"  Run after reboot following 'inject'.",
		cmd_ecc_verify, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(ecc_test, &sub_ecc_test, "ECC flash shim test commands", NULL);
