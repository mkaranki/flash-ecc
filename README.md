# flash-ecc

Zephyr out-of-tree module providing software ECC for NOR flash storage.
Intended for use below a LittleFS filesystem on devices where the flash
hardware does not provide ECC.

## Contents

```
flash-ecc/
├── lib/ecc_crc32/        Portable CRC-32 error detection and correction library
├── drivers/flash/        Zephyr flash shim driver (vaisala,ecc-flash-shim)
├── include/ecc_crc32.h   Public API for the ECC library
├── dts/bindings/         Device tree binding for the shim driver
└── tests/ecc_crc32/      Unit tests for the ECC library (Zephyr ztest)
```

## How it works

Each 256-byte physical NOR flash page is split into 252 bytes of data
payload and a 4-byte CRC-32:

```
┌─────────────────────────────────────────────────┬─────────────────┐
│                 252 bytes data                   │  4 bytes CRC-32 │
└─────────────────────────────────────────────────┴─────────────────┘
 byte 0                                        251  252          255
```

Per 4 KB physical erase sector (16 pages):
- **4032 bytes** usable data visible to upper layers (16 × 252)
- 64 bytes CRC-32 overhead (1.56%)

The CRC-32 uses the standard IEEE polynomial (`0xEDB88320` reflected),
provided by Zephyr's `crc32_ieee()`. For a 252-byte data payload the CRC-32
Hamming distance guarantees:

| Errors | Capability |
|--------|------------|
| 1 bit  | Correct    |
| 2 bits | Correct¹   |
| 3 bits | Detect     |
| 4+ bits| Detect most|

¹ Requires `CONFIG_ECC_CRC32_2BIT_CORRECTION=y` (default off). Without it,
2-bit errors are detected and reported as uncorrectable.

2-bit correction is possible because the 256-byte codeword (252 + 4) is
within the 371-byte HD=5 limit for this polynomial. See
[Koopman's CRC database](https://users.ece.cmu.edu/~koopman/crc/crc32.html)
for details.

The correction algorithm is adapted from
[littlefs-project/ramcrc32bd](https://github.com/littlefs-project/ramcrc32bd).
It avoids recomputing the full CRC for each candidate bit position by
deriving CRC contributions incrementally via the polynomial LFSR shift
relation. The inner loop is pure XOR and shift — no CRC recomputation.

## Zephyr flash shim driver

The shim driver (`vaisala,ecc-flash-shim`) wraps a parent flash device and
presents a virtual flash device to upper layers with:

- `write_block_size` = 252 bytes
- Virtual sector = 4032 bytes (maps 1:1 to a physical 4096-byte erase sector)

All ECC encoding and decoding is transparent. Corrected errors are logged:

- 1-bit correction: `LOG_WRN`
- 2-bit correction: `LOG_ERR` (requires `CONFIG_ECC_CRC32_2BIT_CORRECTION=y`)
- Uncorrectable error: `LOG_ERR`, returns `-EFAULT`

Erased pages (all `0xFF`) are returned as-is without an ECC check.

### LittleFS configuration

Because the virtual sector size differs from the physical sector size,
LittleFS must be configured explicitly using
`FS_LITTLEFS_DECLARE_CUSTOM_CONFIG`:

```c
FS_LITTLEFS_DECLARE_CUSTOM_CONFIG(lfs_data,
    4,    /* alignment  */
    252,  /* read_size  */
    252,  /* prog_size  */
    252,  /* cache_size: one ECC data page = minimum read/write unit */
    512   /* lookahead_size: block_count / 8 (4096 blocks → 512 bytes) */
);
```

**Why the DT fstab geometry must be used explicitly**

The DT fstab geometry properties (`read-size`, `prog-size`, `cache-size`, etc.) ARE read
by Zephyr — but only inside `DEFINE_FS` (`DT_INST_FOREACH_STATUS_OKAY` in
`littlefs_fs.c`), which compiles them into a separate `fs_littlefs` struct and a
`fs_mount_t` registered in the fstab linker section.

When an application mounts the filesystem manually with its own `fs_littlefs` struct
declared via `FS_LITTLEFS_DECLARE_DEFAULT_CONFIG`, that struct receives values from
**Kconfig** (`CONFIG_FS_LITTLEFS_READ_SIZE` = 16, etc.) — not from DT. The DT-sourced
struct and the manually-declared struct are entirely separate objects; the manual mount
ignores the DT values.

The correct approach is to skip the manual struct declaration entirely and reference the
DT-generated mount entry via `FS_FSTAB_DECLARE_ENTRY`:

```c
FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(lfs1));
#define littlefs_mnt FS_FSTAB_ENTRY(DT_NODELABEL(lfs1))

// mount with:
fs_mount(&littlefs_mnt);
```

This way the DT fstab node is the single source of truth for geometry, mount point, and
partition — and `fs_mkfs` can reference the partition via `littlefs_mnt.storage_dev`
instead of a separately-defined `STORAGE_PARTITION_ID`.

With the ECC shim, `prog_size` must be exactly 252 so that a full ECC page (252 bytes
data + 4 bytes CRC) is written atomically. Set this in the DT fstab node and it flows
through automatically.

**Why `alignment = 4`?**
The read and prog buffers are declared `__aligned(alignment)`. Alignment 4 matches the
Cortex-M natural word size for efficient memory access and is compatible with 252-byte
pages (252 % 4 = 0). The DT-driven fstab path also uses `__aligned(4)` for its buffers.

**`block_size`, `block_count`, `block_cycles`** are populated automatically at mount time:
`block_size` from the shim's flash page layout (4032 bytes via `flash_page_foreach`),
`block_count` from partition size / block_size, and `block_cycles` from
`CONFIG_FS_LITTLEFS_BLOCK_CYCLES` (default 512).

**Heap size.** The per-file LittleFS cache heap is auto-sized from
`CONFIG_FS_LITTLEFS_CACHE_SIZE` (default 64), not from the DT fstab `cache-size`. With
`cache-size = <252>` in the fstab, each file open allocates 252 bytes from the heap.
Set `CONFIG_FS_LITTLEFS_FC_HEAP_SIZE` explicitly:

```kconfig
# (252 + 32 overhead) * max_open_files
CONFIG_FS_LITTLEFS_FC_HEAP_SIZE=5120
```

## Integration

### west.yml

Add to your west manifest:

```yaml
- name: flash-ecc
  remote: <your-remote>
  path: modules/lib/flash-ecc
  revision: <commit hash>
```

### Kconfig

```kconfig
CONFIG_FLASH_ECC_SHIM=y
```

This automatically selects `CONFIG_ECC_CRC32` and `CONFIG_FLASH_PAGE_LAYOUT`.

### Device tree

```dts
ecc_flash: ecc-flash-shim {
    compatible = "vaisala,ecc-flash-shim";
    parent-flash = <&your_nor_flash>;
    status = "okay";

    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        /* 4096 virtual sectors × 4032 bytes = 0xFC0000 */
        storage_partition: partition@0 {
            label = "storage";
            reg = <0x0 0xFC0000>;
        };
    };
};
```

## Building and running tests

```bash
west build -b native_sim tests/ecc_crc32
west build -t run
```

## License

BSD 3-Clause. See [LICENSE](LICENSE).

The correction algorithm is adapted from
[littlefs-project/ramcrc32bd](https://github.com/littlefs-project/ramcrc32bd),
Copyright (c) 2024, The littlefs authors, BSD-3-Clause.
