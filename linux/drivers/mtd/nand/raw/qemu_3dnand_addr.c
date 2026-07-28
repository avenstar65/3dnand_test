// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#else
#include <linux/errno.h>
#endif

#include "qemu_3dnand_addr.h"

static int q3n_geometry_valid(const struct q3n_geometry *geo)
{
	return geo && geo->writesize && geo->pages_per_block && geo->blocks;
}

int q3n_page_to_address(const struct q3n_geometry *geo, u32 page,
			u32 *block, u32 *page_in_block)
{
	u64 total_pages;

	if (!q3n_geometry_valid(geo) || !block || !page_in_block)
		return -EINVAL;

	total_pages = (u64)geo->pages_per_block * geo->blocks;
	if ((u64)page >= total_pages)
		return -ERANGE;

	*block = page / geo->pages_per_block;
	*page_in_block = page % geo->pages_per_block;
	return 0;
}

int q3n_offset_to_address(const struct q3n_geometry *geo, u64 offset,
			  u32 *block, u32 *page_in_block, u32 *column)
{
	u64 total_pages;
	u64 total_bytes;
	u64 page;
	int ret;

	if (!q3n_geometry_valid(geo) || !block || !page_in_block || !column)
		return -EINVAL;

	total_pages = (u64)geo->pages_per_block * geo->blocks;
	total_bytes = total_pages * geo->writesize;
	if (offset >= total_bytes)
		return -ERANGE;

	page = offset / geo->writesize;
	ret = q3n_page_to_address(geo, (u32)page, block, page_in_block);
	if (ret)
		return ret;

	*column = offset % geo->writesize;
	return 0;
}
