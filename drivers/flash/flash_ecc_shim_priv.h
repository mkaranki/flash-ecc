/* Internal types shared between flash_ecc_shim.c and flash_ecc_shim_shell.c */
#ifndef FLASH_ECC_SHIM_PRIV_H_
#define FLASH_ECC_SHIM_PRIV_H_

struct ecc_shim_config {
	const struct device *parent;
};

#endif /* FLASH_ECC_SHIM_PRIV_H_ */
