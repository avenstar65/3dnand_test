// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/mtd/rawnand.h>

#include "qemu_3dnand_controller.h"
#include "qemu_3dnand_ecc.h"
#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_priv.h"
#include "qemu_3dnand_regs.h"

static const struct nand_op_instr *
q3n_find_instr(const struct nand_operation *op, enum nand_op_instr_type type,
	       unsigned int occurrence)
{
	unsigned int i;

	for (i = 0; i < op->ninstrs; i++) {
		if (op->instrs[i].type != type)
			continue;
		if (!occurrence)
			return &op->instrs[i];
		occurrence--;
	}

	return NULL;
}

static int q3n_exec_read_id(struct q3n *q3n,
			    const struct nand_operation *op, bool check_only)
{
	const struct nand_op_instr *data;

	data = q3n_find_instr(op, NAND_OP_DATA_IN_INSTR, 0);
	if (!data || !data->ctx.data.len ||
	    data->ctx.data.len > sizeof(q3n->id))
		return -ENOTSUPP;
	if (check_only)
		return 0;

	return q3n_hw_read_id(q3n, data->ctx.data.buf.in,
			      data->ctx.data.len);
}

static int q3n_exec_status(struct q3n *q3n,
			   const struct nand_operation *op, bool check_only)
{
	const struct nand_op_instr *data;

	data = q3n_find_instr(op, NAND_OP_DATA_IN_INSTR, 0);
	if (!data || data->ctx.data.len != 1)
		return -ENOTSUPP;
	if (check_only)
		return 0;

	return q3n_hw_read_status(q3n, data->ctx.data.buf.in);
}

static int q3n_exec_erase(struct q3n *q3n,
			  const struct nand_operation *op, bool check_only)
{
	const struct nand_op_instr *addr;
	u32 row = 0;
	unsigned int i;

	addr = q3n_find_instr(op, NAND_OP_ADDR_INSTR, 0);
	if (!addr || !addr->ctx.addr.naddrs || addr->ctx.addr.naddrs > 3)
		return -ENOTSUPP;

	for (i = 0; i < addr->ctx.addr.naddrs; i++)
		row |= (u32)addr->ctx.addr.addrs[i] << (i * 8);
	if (row % Q3N_PAGES_PER_BLOCK)
		return -EINVAL;
	if (check_only)
		return 0;

	return q3n_hw_erase_block(q3n, row / Q3N_PAGES_PER_BLOCK);
}

static int q3n_exec_op(struct nand_chip *chip,
		       const struct nand_operation *op, bool check_only)
{
	struct q3n *q3n = nand_to_q3n(chip);
	const struct nand_op_instr *command;
	int ret;

	if (!q3n || !op || op->cs)
		return -EINVAL;

	command = q3n_find_instr(op, NAND_OP_CMD_INSTR, 0);
	if (!command)
		return -ENOTSUPP;

	if (!check_only)
		mutex_lock(&q3n->lock);

	switch (command->ctx.cmd.opcode) {
	case NAND_CMD_RESET:
		ret = check_only ? 0 : q3n_hw_reset(q3n);
		break;
	case NAND_CMD_READID:
		ret = q3n_exec_read_id(q3n, op, check_only);
		break;
	case NAND_CMD_STATUS:
		ret = q3n_exec_status(q3n, op, check_only);
		break;
	case NAND_CMD_ERASE1:
		ret = q3n_exec_erase(q3n, op, check_only);
		break;
	default:
		ret = -ENOTSUPP;
		break;
	}

	if (!check_only)
		mutex_unlock(&q3n->lock);
	return ret;
}

static int q3n_attach_chip(struct nand_chip *chip)
{
	return q3n_ecc_init(nand_to_q3n(chip));
}

const struct nand_controller_ops q3n_controller_ops = {
	.attach_chip = q3n_attach_chip,
	.exec_op = q3n_exec_op,
};
