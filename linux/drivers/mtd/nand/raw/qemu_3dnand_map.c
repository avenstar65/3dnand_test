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

bool q3n_program_order_ready(const struct q3n_block_state *state, u32 page)
{
	return state && page == state->next_prog_page;
}

MODULE_DESCRIPTION("QEMU 3D NAND pure address mapper");
MODULE_LICENSE("GPL");
