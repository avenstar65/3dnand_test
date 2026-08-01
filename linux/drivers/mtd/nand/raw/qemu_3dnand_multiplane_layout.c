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

#include "qemu_3dnand_multiplane_layout.h"

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

static int q3n_multiplane_profile_validate(
		const struct q3n_multiplane_profile *profile)
{
	u64 expected;
	u64 logical_blocks;
	u64 logical_pages;
	int ret;

	if (!profile || !q3n_geometry_valid(&profile->physical) ||
	    !q3n_geometry_valid(&profile->logical) ||
	    profile->dies != 2 || profile->planes_per_group != Q3N_MP_PLANES ||
	    !profile->blocks_per_plane || !profile->data_blocks_per_plane ||
	    profile->data_blocks_per_plane > profile->blocks_per_plane ||
	    !profile->pages_per_block ||
	    profile->pages_per_block != profile->physical.pages_per_block ||
	    profile->logical.pages_per_block != profile->pages_per_block)
		return -EINVAL;

	ret = q3n_mul_u64(profile->physical.writesize, Q3N_MP_PLANES,
			  &expected);
	if (ret || expected > UINT_MAX || profile->logical.writesize != expected)
		return ret ? ret : -EINVAL;
	ret = q3n_mul_u64(profile->physical.oobsize, Q3N_MP_PLANES, &expected);
	if (ret || expected > UINT_MAX || profile->logical.oobsize != expected)
		return ret ? ret : -EINVAL;
	ret = q3n_mul_u64(profile->dies, profile->data_blocks_per_plane,
			  &logical_blocks);
	if (ret || logical_blocks > UINT_MAX ||
	    profile->logical.blocks != logical_blocks)
		return ret ? ret : -EINVAL;
	ret = q3n_mul_u64(profile->logical.pages_per_block,
			  profile->logical.blocks, &logical_pages);
	if (ret || logical_pages > UINT_MAX)
		return ret ? ret : -EOVERFLOW;
	ret = q3n_mul_u64(profile->logical.writesize, logical_pages, &expected);
	if (ret || profile->logical_size != expected)
		return ret ? ret : -EINVAL;

	return 0;
}

int q3n_multiplane_layout_build(struct q3n_multiplane_profile *profile,
		const struct q3n_geometry *physical,
		const struct q3n_flash_topology *topology)
{
	struct q3n_multiplane_profile built = { 0 };
	u64 value;
	int ret;

	if (!profile || !q3n_geometry_valid(physical) || !topology)
		return -EINVAL;
	if (topology->dies != 2 || topology->planes_per_die != Q3N_MP_PLANES ||
	    !topology->blocks_per_plane || !topology->data_blocks_per_plane ||
	    topology->data_blocks_per_plane > topology->blocks_per_plane ||
	    !topology->pages_per_block ||
	    topology->pages_per_block != physical->pages_per_block)
		return -EINVAL;

	built.dies = topology->dies;
	built.planes_per_group = topology->planes_per_die;
	built.blocks_per_plane = topology->blocks_per_plane;
	built.data_blocks_per_plane = topology->data_blocks_per_plane;
	built.pages_per_block = topology->pages_per_block;
	built.physical = *physical;
	ret = q3n_mul_u64(physical->writesize, Q3N_MP_PLANES, &value);
	if (ret || value > UINT_MAX)
		return ret ? ret : -EOVERFLOW;
	built.logical.writesize = value;
	ret = q3n_mul_u64(physical->oobsize, Q3N_MP_PLANES, &value);
	if (ret || value > UINT_MAX)
		return ret ? ret : -EOVERFLOW;
	built.logical.oobsize = value;
	built.logical.pages_per_block = physical->pages_per_block;
	ret = q3n_mul_u64(topology->dies, topology->data_blocks_per_plane,
			  &value);
	if (ret || value > UINT_MAX)
		return ret ? ret : -EOVERFLOW;
	built.logical.blocks = value;
	ret = q3n_mul_u64(built.logical.pages_per_block,
			  built.logical.blocks, &value);
	if (ret)
		return ret;
	ret = q3n_mul_u64(built.logical.writesize, value, &built.logical_size);
	if (ret)
		return ret;
	ret = q3n_multiplane_profile_validate(&built);
	if (ret)
		return ret;
	*profile = built;
	return 0;
}

int q3n_multiplane_map_block(const struct q3n_multiplane_profile *profile,
		u32 logical_block, struct q3n_mp_addr *addr)
{
	int ret;

	if (!addr)
		return -EINVAL;
	ret = q3n_multiplane_profile_validate(profile);
	if (ret)
		return ret;
	if (logical_block >= profile->logical.blocks)
		return -ERANGE;

	addr->die = logical_block % profile->dies;
	addr->block_in_plane = logical_block / profile->dies;
	addr->page_in_block = 0;
	return 0;
}

int q3n_multiplane_map_page(const struct q3n_multiplane_profile *profile,
		u32 logical_page, struct q3n_mp_addr *addr)
{
	u64 total_pages;
	u32 logical_block;
	int ret;

	if (!addr)
		return -EINVAL;
	ret = q3n_multiplane_profile_validate(profile);
	if (ret)
		return ret;
	total_pages = (u64)profile->logical.pages_per_block *
		profile->logical.blocks;
	if ((u64)logical_page >= total_pages)
		return -ERANGE;

	logical_block = logical_page / profile->logical.pages_per_block;
	ret = q3n_multiplane_map_block(profile, logical_block, addr);
	if (ret)
		return ret;
	addr->page_in_block = logical_page % profile->logical.pages_per_block;
	return 0;
}

int q3n_multiplane_physical_block(const struct q3n_multiplane_profile *profile,
		const struct q3n_mp_addr *addr, u32 plane, u32 *physical_block)
{
	u64 block;
	int ret;

	if (!addr || !physical_block)
		return -EINVAL;
	ret = q3n_multiplane_profile_validate(profile);
	if (ret)
		return ret;
	if (plane >= Q3N_MP_PLANES || addr->die >= profile->dies ||
	    addr->block_in_plane >= profile->data_blocks_per_plane ||
	    addr->page_in_block >= profile->pages_per_block)
		return -ERANGE;

	ret = q3n_mul_u64(addr->die, profile->planes_per_group, &block);
	if (ret)
		return ret;
	if (block > Q3N_U64_MAX - plane)
		return -EOVERFLOW;
	block += plane;
	ret = q3n_mul_u64(block, profile->blocks_per_plane, &block);
	if (ret || block > Q3N_U64_MAX - addr->block_in_plane)
		return ret ? ret : -EOVERFLOW;
	block += addr->block_in_plane;
	if (block > UINT_MAX)
		return -EOVERFLOW;

	*physical_block = block;
	return 0;
}
