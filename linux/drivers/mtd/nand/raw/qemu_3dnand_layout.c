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
	return data_pages && !(data_pages & (data_pages - 1)) &&
		(data_pages == 2 || data_pages == 4 || data_pages == 8);
}

static int q3n_page_profile_validate(const struct q3n_page_profile *profile)
{
	u64 physical_pages;
	u64 logical_pages;
	u64 expected_size;
	u64 expected_value;
	u64 used_pages;
	u32 expected_stripes;
	int ret;

	if (!profile || !q3n_geometry_valid(&profile->physical) ||
	    !q3n_geometry_valid(&profile->logical))
		return -EINVAL;

	ret = q3n_mul_u64(profile->physical.pages_per_block,
			  profile->physical.blocks, &physical_pages);
	if (ret || physical_pages > UINT_MAX)
		return -EOVERFLOW;
	ret = q3n_mul_u64(profile->logical.pages_per_block,
			  profile->logical.blocks, &logical_pages);
	if (ret || logical_pages > UINT_MAX)
		return -EOVERFLOW;

	if (!profile->raid_enabled) {
		if (profile->data_pages != 1 || profile->parity_pages ||
		    profile->stripe_pages != 1 ||
		    profile->stripes_per_block !=
			    profile->physical.pages_per_block ||
		    profile->tail_pages ||
		    profile->logical.writesize != profile->physical.writesize ||
		    profile->logical.oobsize != profile->physical.oobsize ||
		    profile->logical.pages_per_block !=
			    profile->physical.pages_per_block ||
		    profile->logical.blocks != profile->physical.blocks)
			return -EINVAL;
	} else {
		if (!q3n_raid_data_pages_valid(profile->data_pages) ||
		    profile->parity_pages != 1 ||
		    profile->stripe_pages != profile->data_pages + 1)
			return -EINVAL;

		expected_stripes = profile->physical.pages_per_block /
			profile->stripe_pages;
		if (!expected_stripes ||
		    profile->stripes_per_block != expected_stripes ||
		    profile->logical.pages_per_block != expected_stripes ||
		    profile->logical.blocks != profile->physical.blocks)
			return -EINVAL;

		ret = q3n_mul_u64(profile->stripes_per_block,
				  profile->stripe_pages, &used_pages);
		if (ret || used_pages > profile->physical.pages_per_block ||
		    profile->tail_pages !=
			    profile->physical.pages_per_block - used_pages)
			return -EINVAL;

		ret = q3n_mul_u64(profile->physical.writesize,
				  profile->data_pages, &expected_value);
		if (ret || expected_value > UINT_MAX)
			return -EOVERFLOW;
		if (profile->logical.writesize != expected_value)
			return -EINVAL;

		ret = q3n_mul_u64(profile->physical.oobsize,
				  profile->data_pages, &expected_value);
		if (ret || expected_value > UINT_MAX)
			return -EOVERFLOW;
		if (profile->logical.oobsize != expected_value)
			return -EINVAL;
	}

	ret = q3n_mul_u64(profile->logical.writesize, logical_pages,
			  &expected_size);
	if (ret)
		return ret;
	if (profile->logical_size != expected_size)
		return -EINVAL;

	return 0;
}

int q3n_layout_build(struct q3n_page_profile *profile,
			     const struct q3n_geometry *physical,
			     bool raid_enabled, u32 data_pages)
{
	struct q3n_page_profile built = { 0 };
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

	built.raid_enabled = raid_enabled;
	built.data_pages = data_pages;
	built.parity_pages = raid_enabled ? 1 : 0;
	built.stripe_pages = stripe_pages;
	built.physical = *physical;

	if (!raid_enabled) {
		built.stripes_per_block = physical->pages_per_block;
		built.tail_pages = 0;
		built.logical = *physical;
		ret = q3n_mul_u64(physical->pages_per_block, physical->blocks,
				  &value);
		if (ret)
			return ret;
		ret = q3n_mul_u64(physical->writesize, value,
				  &built.logical_size);
		if (ret)
			return ret;
		ret = q3n_page_profile_validate(&built);
		if (ret)
			return ret;
		*profile = built;
		return 0;
	}

	built.stripes_per_block = physical->pages_per_block / stripe_pages;
	built.tail_pages = physical->pages_per_block -
		built.stripes_per_block * stripe_pages;
	if (!built.stripes_per_block)
		return -EINVAL;

	ret = q3n_mul_u64(physical->writesize, data_pages, &value);
	if (ret || value > UINT_MAX)
		return -EOVERFLOW;
	built.logical.writesize = value;
	ret = q3n_mul_u64(physical->oobsize, data_pages, &value);
	if (ret || value > UINT_MAX)
		return -EOVERFLOW;
	built.logical.oobsize = value;
	built.logical.pages_per_block = built.stripes_per_block;
	built.logical.blocks = physical->blocks;

	ret = q3n_mul_u64(built.logical.writesize,
			  built.logical.pages_per_block, &value);
	if (ret)
		return ret;
	ret = q3n_mul_u64(value, built.logical.blocks, &built.logical_size);
	if (ret)
		return ret;
	ret = q3n_page_profile_validate(&built);
	if (ret)
		return ret;
	*profile = built;
	return 0;
}

int q3n_layout_map_page(const struct q3n_page_profile *profile,
				u32 logical_page, struct q3n_page_map *map)
{
	u64 block_end;
	u64 block_start;
	u64 total_pages;
	u64 first_data_page;
	u64 parity_page;
	int ret;

	if (!map)
		return -EINVAL;
	ret = q3n_page_profile_validate(profile);
	if (ret)
		return ret;

	total_pages = (u64)profile->logical.pages_per_block *
		profile->logical.blocks;
	if ((u64)logical_page >= total_pages)
		return -ERANGE;

	map->logical_block = logical_page / profile->logical.pages_per_block;
	map->stripe_in_block = logical_page % profile->logical.pages_per_block;
	block_start = (u64)map->logical_block *
		profile->physical.pages_per_block;
	block_end = block_start + profile->physical.pages_per_block;
	first_data_page = block_start +
		(u64)map->stripe_in_block * profile->stripe_pages;
	parity_page = profile->raid_enabled ?
		first_data_page + profile->data_pages : first_data_page;
	if (first_data_page > UINT_MAX || parity_page > UINT_MAX)
		return -EOVERFLOW;
	if (first_data_page < block_start ||
	    first_data_page + profile->data_pages > block_end ||
	    parity_page >= block_end)
		return -ERANGE;

	map->first_data_page = first_data_page;
	map->parity_page = parity_page;
	return 0;
}
