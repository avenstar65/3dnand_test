// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#include <limits.h>
#include <string.h>
#else
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#endif

#include "qemu_3dnand_flash.h"

const struct q3n_flash_info *q3n_flash_info_for_id(const u8 *id, size_t len)
{
	const struct q3n_flash_info *info = ytmc_nand_flash_info();

	if (!id || len != YTMC_Q3N_ID_LEN)
		return NULL;

	for (; info->nand.name; info++) {
		if (info->nand.id_len == len && !memcmp(id, info->nand.id, len))
			return info;
	}

	return NULL;
}

int q3n_flash_build_scan_ids(struct nand_flash_dev scan_ids[2],
			     const struct nand_flash_dev *physical_ids,
			     const struct q3n_page_profile *profile)
{
	u64 erasesize;
	u64 chipsize;

	if (!scan_ids || !physical_ids || !profile)
		return -EINVAL;
	if (!profile->logical.writesize || !profile->logical.oobsize ||
	    !profile->logical.pages_per_block ||
	    !profile->logical_size || profile->logical.oobsize > USHRT_MAX ||
	    profile->logical_size % (1024 * 1024))
		return -EINVAL;

	erasesize = (u64)profile->logical.writesize *
		profile->logical.pages_per_block;
	chipsize = profile->logical_size / (1024 * 1024);
	if (erasesize > UINT_MAX || chipsize > UINT_MAX)
		return -EOVERFLOW;

	scan_ids[0] = physical_ids[0];
	scan_ids[0].pagesize = profile->logical.writesize;
	scan_ids[0].oobsize = profile->logical.oobsize;
	scan_ids[0].erasesize = erasesize;
	scan_ids[0].chipsize = chipsize;
	memset(&scan_ids[1], 0, sizeof(scan_ids[1]));
	return 0;
}
