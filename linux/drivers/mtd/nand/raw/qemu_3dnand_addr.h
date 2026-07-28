/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_ADDR_H
#define QEMU_3DNAND_ADDR_H

#ifdef Q3N_HOST_TEST
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include <linux/types.h>
#endif

struct q3n_geometry {
	u32 writesize;
	u32 oobsize;
	u32 pages_per_block;
	u32 blocks;
};

int q3n_page_to_address(const struct q3n_geometry *geo, u32 page,
			u32 *block, u32 *page_in_block);
int q3n_offset_to_address(const struct q3n_geometry *geo, u64 offset,
			  u32 *block, u32 *page_in_block, u32 *column);

#endif
