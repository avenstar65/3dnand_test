// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/module.h>

#include "qemu_3dnand_priv.h"

static int q3n_validate_geometry(const struct q3n_geometry *geometry)
{
	if (!geometry || !geometry->pages_per_block ||
	    !geometry->data_pages_per_stripe || !geometry->data_block_count ||
	    !geometry->parity_block_count)
		return -EINVAL;

	if (geometry->data_block_count % geometry->data_pages_per_stripe)
		return -EINVAL;

	if (geometry->parity_block_count <
	    geometry->data_block_count / geometry->data_pages_per_stripe)
		return -ERANGE;

	return 0;
}

int q3n_map_data_page(const struct q3n_geometry *geometry, u64 stripe,
		      u8 slot, struct q3n_phys_addr *out)
{
	u32 page;
	u64 group;
	u64 block;
	int ret;

	ret = q3n_validate_geometry(geometry);
	if (ret)
		return ret;
	if (!out || slot >= geometry->data_pages_per_stripe)
		return -EINVAL;

	group = div_u64_rem(stripe, geometry->pages_per_block, &page);
	block = group * geometry->data_pages_per_stripe + slot;
	if (block >= geometry->data_block_count)
		return -ERANGE;

	out->block = block;
	out->page = page;
	return 0;
}

int q3n_map_parity_page(const struct q3n_geometry *geometry, u64 stripe,
			struct q3n_phys_addr *out)
{
	u32 page;
	u64 group;
	int ret;

	ret = q3n_validate_geometry(geometry);
	if (ret)
		return ret;
	if (!out)
		return -EINVAL;

	group = div_u64_rem(stripe, geometry->pages_per_block, &page);
	if (group >= geometry->parity_block_count)
		return -ERANGE;

	out->block = geometry->data_block_count + group;
	out->page = page;
	return 0;
}

int q3n_map_serial_data_page(const struct q3n_geometry *geometry,
			     u64 logical_page, struct q3n_phys_addr *out)
{
	u64 logical_pages_per_block;
	u64 block;
	u64 page_in_block;
	u32 stripe;
	u32 slot;
	int ret;

	ret = q3n_validate_geometry(geometry);
	if (ret)
		return ret;
	if (!out)
		return -EINVAL;

	logical_pages_per_block =
		(geometry->pages_per_block /
		 (geometry->data_pages_per_stripe + 1)) *
		geometry->data_pages_per_stripe;
	if (!logical_pages_per_block)
		return -ERANGE;

	block = div64_u64_rem(logical_page, logical_pages_per_block,
				    &page_in_block);
	if (block >= geometry->data_block_count)
		return -ERANGE;
	stripe = div_u64_rem(page_in_block,
			    geometry->data_pages_per_stripe, &slot);
	out->block = block;
	out->page = stripe * (geometry->data_pages_per_stripe + 1) + slot;
	return 0;
}

MODULE_DESCRIPTION("QEMU 3D NAND pure address mapper");
MODULE_LICENSE("GPL");
