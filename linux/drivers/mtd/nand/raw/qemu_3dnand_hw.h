/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_HW_H
#define QEMU_3DNAND_HW_H

#include "qemu_3dnand_priv.h"

u32 q3n_hw_reg_read(struct q3n *q3n, u32 reg);
void q3n_hw_reg_write(struct q3n *q3n, u32 reg, u32 value);
int q3n_hw_wait_ready_status(struct q3n *q3n, u32 *status);
void q3n_hw_read_window(struct q3n *q3n, void *buffer, size_t length);
void q3n_hw_write_window(struct q3n *q3n, const void *buffer, size_t length);
u32 q3n_hw_read_capabilities(struct q3n *q3n);

int q3n_hw_reset(struct q3n *q3n);
int q3n_hw_read_id(struct q3n *q3n, u8 *id, size_t len);
int q3n_hw_read_page(struct q3n *q3n, u32 page, void *data, void *oob,
		     bool raw, struct q3n_ecc_result *result);
int q3n_hw_program_page(struct q3n *q3n, u32 page,
			const void *data, const void *oob);
int q3n_hw_read_oob(struct q3n *q3n, u32 page, void *oob);
int q3n_hw_program_oob(struct q3n *q3n, u32 page, const void *oob);
int q3n_hw_erase_block(struct q3n *q3n, u32 block);
int q3n_hw_set_retry_mode(struct q3n *q3n, unsigned int mode);
int q3n_hw_read_status(struct q3n *q3n, u8 *status);

#endif
