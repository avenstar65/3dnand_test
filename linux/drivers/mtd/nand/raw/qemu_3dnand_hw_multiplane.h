/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_HW_MULTIPLANE_H
#define QEMU_3DNAND_HW_MULTIPLANE_H

#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_multiplane_layout.h"

struct q3n_mp_plane_result {
	int status;
	struct q3n_ecc_result ecc;
};

struct q3n_mp_result {
	u8 done_mask;
	u8 fail_mask;
	struct q3n_mp_plane_result plane[Q3N_MP_PLANES];
};

int q3n_hw_mp_read_page(struct q3n *q3n, const struct q3n_mp_addr *addr,
			void *data, bool raw, struct q3n_mp_result *result);
int q3n_hw_mp_program_page(struct q3n *q3n, const struct q3n_mp_addr *addr,
			   const void *data, struct q3n_mp_result *result);
int q3n_hw_mp_read_oob(struct q3n *q3n, const struct q3n_mp_addr *addr,
		       void *oob, struct q3n_mp_result *result);
int q3n_hw_mp_program_oob(struct q3n *q3n, const struct q3n_mp_addr *addr,
			  const void *oob, struct q3n_mp_result *result);
int q3n_hw_mp_erase_group(struct q3n *q3n, const struct q3n_mp_addr *addr,
			  struct q3n_mp_result *result);

#endif
