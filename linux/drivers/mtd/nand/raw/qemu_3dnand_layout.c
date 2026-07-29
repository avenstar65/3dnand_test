// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#define Q3N_U64_MAX UINT64_MAX
#else
#include <linux/errno.h>
#include <linux/limits.h>
#define Q3N_U64_MAX U64_MAX
#endif

#include "qemu_3dnand_layout.h"

static int q3n_mul_u64(u64 left, u64 right, u64 *result)
{
	if (left && right > Q3N_U64_MAX / left)
		return -EOVERFLOW;

	*result = left * right;
	return 0;
}

static int q3n_geometry_valid(const struct q3n_geometry *geometry)
{
	return geometry && geometry->writesize && geometry->oobsize &&
		geometry->pages_per_block && geometry->blocks;
}

static int q3n_raid_data_pages_valid(u32 data_pages)
{
	return data_pages == 2 || data_pages == 4 || data_pages == 8;
}

int q3n_layout_build(struct q3n_page_profile *profile,
			     const struct q3n_geometry *physical,
			     bool raid_enabled, u32 data_pages)
{
	u64 physical_pages;
	u64 logical_pages;
	u64 value;
	u32 stripe_pages;
	int ret;

	if (!profile || !q3n_geometry_valid(physical))
		return -EINVAL;
	if (raid_enabled && !q3n_raid_data_pages_valid(data_pages))
		return -EINVAL;

	if (!raid_enabled)
		data_pages = 1;
	stripe_pages = data_pages + (raid_enabled ? 1 : 0);

	ret = q3n_mul_u64(physical->pages_per_block, physical->blocks,
			  &physical_pages);
	if (ret)
		return ret;
	if (physical_pages > UINT_MAX)
		return -EOVERFLOW;

	profile->raid_enabled = raid_enabled;
	profile->data_pages = data_pages;
	profile->parity_pages = raid_enabled ? 1 : 0;
	profile->stripe_pages = stripe_pages;
	profile->physical = *physical;

	if (!raid_enabled) {
		profile->stripes_per_block = physical->pages_per_block;
		profile->tail_pages = 0;
		profile->logical = *physical;
		ret = q3n_mul_u64(physical->writesize, physical_pages,
				  &profile->logical_size);
		return ret;
	}

	profile->stripes_per_block = physical->pages_per_block / stripe_pages;
	profile->tail_pages = physical->pages_per_block -
		profile->stripes_per_block * stripe_pages;
	if (!profile->stripes_per_block)
		return -EINVAL;

	ret = q3n_mul_u64(physical->writesize, data_pages, &value);
	if (ret || value > UINT_MAX)
		return -EOVERFLOW;
	profile->logical.writesize = value;
	ret = q3n_mul_u64(physical->oobsize, data_pages, &value);
	if (ret || value > UINT_MAX)
		return -EOVERFLOW;
	profile->logical.oobsize = value;
	profile->logical.pages_per_block = profile->stripes_per_block;
	profile->logical.blocks = physical->blocks;

	ret = q3n_mul_u64(profile->logical.pages_per_block,
			  profile->logical.blocks, &logical_pages);
	if (ret || logical_pages > UINT_MAX)
		return -EOVERFLOW;
	ret = q3n_mul_u64(profile->logical.writesize,
			  profile->logical.pages_per_block, &value);
	if (ret)
		return ret;
	return q3n_mul_u64(value, profile->logical.blocks, &profile->logical_size);
}

int q3n_layout_map_page(const struct q3n_page_profile *profile,
				u32 logical_page, struct q3n_page_map *map)
{
	u64 total_pages;
	u64 first_data_page;

	if (!profile || !map || !q3n_geometry_valid(&profile->physical) ||
	    !q3n_geometry_valid(&profile->logical) || !profile->stripe_pages ||
	    !profile->stripes_per_block)
		return -EINVAL;

	total_pages = (u64)profile->logical.pages_per_block *
		profile->logical.blocks;
	if ((u64)logical_page >= total_pages)
		return -ERANGE;

	map->logical_block = logical_page / profile->logical.pages_per_block;
	map->stripe_in_block = logical_page % profile->logical.pages_per_block;
	first_data_page = (u64)map->logical_block *
		profile->physical.pages_per_block +
		(u64)map->stripe_in_block * profile->stripe_pages;
	if (first_data_page > UINT_MAX ||
	    first_data_page + profile->stripe_pages - 1 > UINT_MAX)
		return -EOVERFLOW;

	map->first_data_page = first_data_page;
	map->parity_page = first_data_page + profile->data_pages;
	if (!profile->raid_enabled)
		map->parity_page = map->first_data_page;
	return 0;
}
