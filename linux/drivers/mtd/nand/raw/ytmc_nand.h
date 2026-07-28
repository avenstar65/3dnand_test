/* SPDX-License-Identifier: GPL-2.0 */
#ifndef YTMC_NAND_H
#define YTMC_NAND_H

#define YTMC_Q3N_ID_LEN 8

#ifdef Q3N_HOST_TEST
#include <stdint.h>

#define NAND_NON_POWER_OF_2_GEOMETRY (1U << 15)
#define NAND_NO_SUBPAGE_WRITE (1U << 9)

struct nand_flash_dev {
	char *name;
	union {
		struct {
			uint8_t mfr_id;
			uint8_t dev_id;
		};
		uint8_t id[8];
	};
	unsigned int pagesize;
	unsigned int chipsize;
	unsigned int erasesize;
	unsigned int options;
	uint16_t id_len;
	uint16_t oobsize;
	struct {
		uint16_t strength_ds;
		uint16_t step_ds;
	} ecc;
};
#else
#include <linux/mtd/rawnand.h>
#endif

struct nand_flash_dev *ytmc_nand_ids(void);

#endif
