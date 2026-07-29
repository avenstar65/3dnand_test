/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_LAYOUT_H
#define QEMU_3DNAND_LAYOUT_H

#ifdef Q3N_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include <linux/types.h>
#endif

#include "qemu_3dnand_addr.h"

struct q3n_page_profile {
	bool raid_enabled;
	u32 data_pages;
	u32 parity_pages;
	u32 stripe_pages;
	u32 stripes_per_block;
	u32 tail_pages;
	struct q3n_geometry physical;
	struct q3n_geometry logical;
	u64 logical_size;
};

struct q3n_page_map {
	u32 logical_block;
	u32 stripe_in_block;
	u32 first_data_page;
	u32 parity_page;
};

int q3n_layout_build(struct q3n_page_profile *profile,
			     const struct q3n_geometry *physical,
			     bool raid_enabled, u32 data_pages);
int q3n_layout_map_page(const struct q3n_page_profile *profile,
				u32 logical_page, struct q3n_page_map *map);

#endif
