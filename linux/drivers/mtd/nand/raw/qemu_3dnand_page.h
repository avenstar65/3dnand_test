/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_PAGE_H
#define QEMU_3DNAND_PAGE_H

#include "qemu_3dnand_priv.h"

struct q3n_page_result {
	u32 max_bitflips;
	u32 corrected_bits;
	u32 failed_data_pages;
	u8 failed_plane_mask;
	bool parity_failed;
	bool recovered;
};

struct q3n_page_ops {
	int (*read_page)(struct q3n *q3n, u32 logical_page,
			 void *data, void *oob, bool raw,
			 struct q3n_page_result *result);
	int (*write_page)(struct q3n *q3n, u32 logical_page,
			  const void *data, const void *oob);
	int (*read_oob)(struct q3n *q3n, u32 logical_page, void *oob);
	int (*write_oob)(struct q3n *q3n, u32 logical_page,
			 const void *oob);
	int (*erase_block)(struct q3n *q3n, u32 logical_block);
};

bool q3n_page_buffer_erased(const void *buffer, size_t length);
int q3n_page_layer_init(struct q3n *q3n, enum q3n_storage_mode mode,
			u32 raid_data_pages,
			const struct q3n_flash_topology *topology);
void q3n_page_layer_cleanup(struct q3n *q3n);
int q3n_page_read(struct q3n *q3n, u32 logical_page,
		  void *data, void *oob, bool raw,
		  struct q3n_page_result *result);
int q3n_page_write(struct q3n *q3n, u32 logical_page,
		   const void *data, const void *oob);
int q3n_page_read_oob(struct q3n *q3n, u32 logical_page, void *oob);
int q3n_page_write_oob(struct q3n *q3n, u32 logical_page, const void *oob);
int q3n_page_erase_block(struct q3n *q3n, u32 logical_block);

#endif
