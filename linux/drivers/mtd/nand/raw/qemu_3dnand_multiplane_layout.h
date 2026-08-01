/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_MULTIPLANE_LAYOUT_H
#define QEMU_3DNAND_MULTIPLANE_LAYOUT_H

#include "qemu_3dnand_addr.h"
#include "ytmc_nand.h"

#define Q3N_MP_PLANES 4U

struct q3n_multiplane_profile {
	u32 dies;
	u32 planes_per_group;
	u32 blocks_per_plane;
	u32 data_blocks_per_plane;
	u32 pages_per_block;
	struct q3n_geometry physical;
	struct q3n_geometry logical;
	u64 logical_size;
};

struct q3n_mp_addr {
	u32 die;
	u32 block_in_plane;
	u32 page_in_block;
};

int q3n_multiplane_layout_build(struct q3n_multiplane_profile *profile,
		const struct q3n_geometry *physical,
		const struct q3n_flash_topology *topology);
int q3n_multiplane_map_page(const struct q3n_multiplane_profile *profile,
		u32 logical_page, struct q3n_mp_addr *addr);
int q3n_multiplane_map_block(const struct q3n_multiplane_profile *profile,
		u32 logical_block, struct q3n_mp_addr *addr);
int q3n_multiplane_physical_block(const struct q3n_multiplane_profile *profile,
		const struct q3n_mp_addr *addr, u32 plane, u32 *physical_block);

#endif
