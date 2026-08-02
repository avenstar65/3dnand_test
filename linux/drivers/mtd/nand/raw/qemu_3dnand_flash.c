// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#include <limits.h>
#include <string.h>
#define Q3N_U64_MAX UINT64_MAX
#else
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#define Q3N_U64_MAX U64_MAX
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

static int q3n_flash_mul_u64(u64 left, u64 right, u64 *result)
{
	if (left && right > Q3N_U64_MAX / left)
		return -EOVERFLOW;

	*result = left * right;
	return 0;
}

int q3n_flash_build_scan_ids(struct nand_flash_dev scan_ids[2],
			     const struct q3n_flash_info *info,
			     const struct q3n_geometry *logical)
{
	u64 erasesize;
	u64 size;
	u64 chipsize;
	int ret;

	if (!scan_ids || !info || !logical)
		return -EINVAL;
	if (!logical->writesize || !logical->oobsize ||
	    !logical->pages_per_block || !logical->blocks ||
	    logical->oobsize > USHRT_MAX)
		return -EINVAL;

	ret = q3n_flash_mul_u64(logical->writesize, logical->pages_per_block,
				&erasesize);
	if (ret)
		return ret;
	ret = q3n_flash_mul_u64(erasesize, logical->blocks, &size);
	if (ret)
		return ret;
	if (size % (1024 * 1024))
		return -EINVAL;
	chipsize = size / (1024 * 1024);
	if (erasesize > UINT_MAX || chipsize > UINT_MAX)
		return -EOVERFLOW;

	scan_ids[0] = info->nand;
	scan_ids[0].pagesize = logical->writesize;
	scan_ids[0].oobsize = logical->oobsize;
	scan_ids[0].erasesize = erasesize;
	scan_ids[0].chipsize = chipsize;
	memset(&scan_ids[1], 0, sizeof(scan_ids[1]));
	return 0;
}
