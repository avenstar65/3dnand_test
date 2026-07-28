// SPDX-License-Identifier: GPL-2.0
#include "ytmc_nand.h"

static struct nand_flash_dev ytmc_ids[] = {
	{
		.name = "YTMC Q3N 40.625GiB 8-bit",
		.id = { 0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44 },
		.pagesize = 16384,
		.chipsize = 41600,
		.erasesize = 26214400,
		.options = NAND_NO_SUBPAGE_WRITE |
			   NAND_NON_POWER_OF_2_GEOMETRY,
		.id_len = YTMC_Q3N_ID_LEN,
		.oobsize = 1024,
		.ecc = {
			.strength_ds = 40,
			.step_ds = 1024,
		},
	},
	{ }
};

struct nand_flash_dev *ytmc_nand_ids(void)
{
	return ytmc_ids;
}
