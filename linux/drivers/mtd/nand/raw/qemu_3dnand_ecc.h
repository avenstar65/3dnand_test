/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_ECC_H
#define QEMU_3DNAND_ECC_H

#include "qemu_3dnand_priv.h"

struct q3n_ecc_stats {
	u32 corrected;
	u32 failed;
};

int q3n_ecc_account_result(const struct q3n_ecc_result *result,
			   unsigned int retry_mode,
			   struct q3n_ecc_stats *stats);

#ifndef Q3N_HOST_TEST
int q3n_ecc_init(struct q3n *q3n);
int q3n_ecc_read_page(struct nand_chip *chip, u8 *buf,
		      int oob_required, int page);
int q3n_ecc_write_page(struct nand_chip *chip, const u8 *buf,
		       int oob_required, int page);
int q3n_ecc_read_page_raw(struct nand_chip *chip, u8 *buf,
			  int oob_required, int page);
int q3n_ecc_write_page_raw(struct nand_chip *chip, const u8 *buf,
			   int oob_required, int page);
int q3n_ecc_read_oob(struct nand_chip *chip, int page);
int q3n_ecc_write_oob(struct nand_chip *chip, int page);
int q3n_setup_read_retry(struct nand_chip *chip, int retry_mode);
#endif

#endif
