/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_CONTROLLER_H
#define QEMU_3DNAND_CONTROLLER_H

#ifdef Q3N_HOST_TEST
#include "qemu_3dnand_priv.h"
#else
#include <linux/mtd/rawnand.h>
#endif

struct q3n;

#ifndef Q3N_HOST_TEST
extern const struct nand_controller_ops q3n_controller_ops;
void q3n_controller_legacy_init(struct nand_chip *chip);
#endif

void q3n_legacy_state_init(struct q3n *q3n);
void q3n_legacy_command(struct q3n *q3n, unsigned int command,
			int column, int page_addr);
u8 q3n_legacy_read_byte_value(struct q3n *q3n);
void q3n_legacy_read_buffer(struct q3n *q3n, u8 *buf, int len);
void q3n_legacy_write_buffer(struct q3n *q3n, const u8 *buf, int len);
int q3n_legacy_wait(struct q3n *q3n);
int q3n_legacy_select(struct q3n *q3n, int chipnr);

#endif
