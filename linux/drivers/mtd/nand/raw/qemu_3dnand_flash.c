// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <string.h>
#else
#include <linux/string.h>
#endif

#include "qemu_3dnand_flash.h"

struct nand_flash_dev *q3n_flash_ids_for_id(const u8 *id, size_t len)
{
	struct nand_flash_dev *ids;

	if (!id || len != YTMC_Q3N_ID_LEN)
		return NULL;

	ids = ytmc_nand_ids();
	if (ids[0].id_len != len || memcmp(id, ids[0].id, len))
		return NULL;

	return ids;
}
