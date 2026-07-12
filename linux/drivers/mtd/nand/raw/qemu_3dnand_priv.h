/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QEMU_3DNAND_PRIV_H
#define __QEMU_3DNAND_PRIV_H

#include <linux/types.h>

struct q3n_geometry {
	u32 page_size;
	u32 pages_per_block;
	u32 data_pages_per_stripe;
	u32 data_block_count;
	u32 parity_block_count;
};

struct q3n_phys_addr {
	u32 block;
	u32 page;
};

struct q3n_block_state {
	u32 next_prog_page;
};

int q3n_map_data_page(const struct q3n_geometry *geometry, u64 stripe,
		      u8 slot, struct q3n_phys_addr *out);
int q3n_map_parity_page(const struct q3n_geometry *geometry, u64 stripe,
			struct q3n_phys_addr *out);
bool q3n_program_order_ready(const struct q3n_block_state *state, u32 page);

#endif /* __QEMU_3DNAND_PRIV_H */
