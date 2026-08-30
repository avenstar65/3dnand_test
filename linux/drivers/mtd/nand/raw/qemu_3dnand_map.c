// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/string.h>

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

static int q3n_validate_raid_geometry(const struct q3n_geometry *geometry)
{
	if (!geometry || !geometry->page_size || !geometry->pages_per_block ||
	    !geometry->blocks_per_plane || !geometry->data_blocks_per_plane ||
	    geometry->data_blocks_per_plane > geometry->blocks_per_plane ||
	    geometry->dies != 2 || geometry->planes_per_die != 4)
		return -EINVAL;
	if (geometry->raid_level != Q3N_RAID1 &&
	    geometry->raid_level != Q3N_RAID5)
		return -EINVAL;
	return 0;
}

static int q3n_set_member(const struct q3n_geometry *geometry,
			  struct q3n_raid_group *group, u8 plane)
{
	u64 lane = (u64)group->die * geometry->planes_per_die + plane;
	u64 block;

	if (check_mul_overflow(lane, (u64)geometry->blocks_per_plane, &block) ||
	    check_add_overflow(block, (u64)group->block_in_plane, &block) ||
	    block > U32_MAX)
		return -EOVERFLOW;
	group->member[plane].block = block;
	group->member[plane].page = group->page;
	return 0;
}

int q3n_map_raid1_page(const struct q3n_geometry *geometry, u64 leb,
		       u32 page, struct q3n_raid_group *out)
{
	u64 max_lebs;
	u64 stripe_id;
	u8 set;
	u8 first_plane;
	int ret;

	ret = q3n_validate_raid_geometry(geometry);
	if (ret)
		return ret;
	if (!out || geometry->raid_level != Q3N_RAID1 ||
	    page >= geometry->pages_per_block)
		return -EINVAL;
	if (check_mul_overflow((u64)geometry->data_blocks_per_plane, 4ULL,
			       &max_lebs) || leb >= max_lebs)
		return -ERANGE;
	if (check_mul_overflow(leb, (u64)geometry->pages_per_block,
			       &stripe_id) ||
	    check_add_overflow(stripe_id, (u64)page, &stripe_id))
		return -EOVERFLOW;

	memset(out, 0, sizeof(*out));
	set = leb % 4;
	out->die = set / 2;
	first_plane = (set % 2) * 2;
	out->block_in_plane = div_u64(leb, 4);
	out->page = page;
	out->stripe_id = stripe_id;
	out->member_mask = BIT(first_plane) | BIT(first_plane + 1);
	out->parity_plane = 0xff;
	out->data_pages = 1;
	ret = q3n_set_member(geometry, out, first_plane);
	if (ret)
		return ret;
	return q3n_set_member(geometry, out, first_plane + 1);
}

int q3n_map_raid5_stripe(const struct q3n_geometry *geometry, u64 leb,
			 u32 page, struct q3n_raid_group *out)
{
	u64 max_lebs;
	u64 stripe_id;
	int ret;
	u8 plane;

	ret = q3n_validate_raid_geometry(geometry);
	if (ret)
		return ret;
	if (!out || geometry->raid_level != Q3N_RAID5 ||
	    page >= geometry->pages_per_block)
		return -EINVAL;
	if (check_mul_overflow((u64)geometry->data_blocks_per_plane, 2ULL,
			       &max_lebs) || leb >= max_lebs)
		return -ERANGE;
	if (check_mul_overflow(leb, (u64)geometry->pages_per_block,
			       &stripe_id) ||
	    check_add_overflow(stripe_id, (u64)page, &stripe_id))
		return -EOVERFLOW;

	memset(out, 0, sizeof(*out));
	out->die = leb % 2;
	out->block_in_plane = div_u64(leb, 2);
	out->page = page;
	out->stripe_id = stripe_id;
	out->member_mask = 0x0f;
	out->parity_plane = stripe_id % 4;
	out->data_pages = 3;
	for (plane = 0; plane < 4; plane++) {
		ret = q3n_set_member(geometry, out, plane);
		if (ret)
			return ret;
	}
	return 0;
}

int q3n_raid_geometry_values(const struct q3n_geometry *geometry,
			     u32 *writesize, u32 *erasesize, u64 *size)
{
	u32 data_pages;
	u64 erase;
	u64 lebs;

	if (q3n_validate_raid_geometry(geometry) || !writesize || !erasesize ||
	    !size)
		return -EINVAL;
	data_pages = geometry->raid_level == Q3N_RAID1 ? 1 : 3;
	if (check_mul_overflow(geometry->page_size, data_pages, writesize) ||
	    check_mul_overflow((u64)*writesize,
			       (u64)geometry->pages_per_block, &erase) ||
	    erase > U32_MAX)
		return -EOVERFLOW;
	*erasesize = erase;
	lebs = geometry->raid_level == Q3N_RAID1 ? 4 : 2;
	if (check_mul_overflow(lebs,
			       (u64)geometry->data_blocks_per_plane, &lebs) ||
	    check_mul_overflow(lebs, erase, size))
		return -EOVERFLOW;
	return 0;
}

MODULE_DESCRIPTION("QEMU 3D NAND pure address mapper");
MODULE_LICENSE("GPL");
