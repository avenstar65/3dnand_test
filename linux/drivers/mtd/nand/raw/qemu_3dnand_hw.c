// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#include <string.h>
#else
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#endif

#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_regs.h"

u32 q3n_hw_reg_read(struct q3n *q3n, u32 reg)
{
#ifdef Q3N_HOST_TEST
	return q3n->mmio.read(q3n->mmio.context, reg);
#else
	return readl(q3n->regs + reg);
#endif
}

void q3n_hw_reg_write(struct q3n *q3n, u32 reg, u32 value)
{
#ifdef Q3N_HOST_TEST
	q3n->mmio.write(q3n->mmio.context, reg, value);
#else
	writel(value, q3n->regs + reg);
#endif
}

int q3n_hw_wait_ready_status(struct q3n *q3n, u32 *status)
{
	unsigned int attempt;

	if (!q3n || !status)
		return -EINVAL;

	for (attempt = 0; attempt < 1000; attempt++) {
		u32 value = q3n_hw_reg_read(q3n, Q3N_REG_STATUS);

		if (value & Q3N_STATUS_READY) {
			*status = value;
			return 0;
		}
#ifndef Q3N_HOST_TEST
		udelay(1);
#endif
	}

	return -ETIMEDOUT;
}

static int q3n_hw_wait_ready(struct q3n *q3n)
{
	u32 status;
	int ret;

	ret = q3n_hw_wait_ready_status(q3n, &status);
	if (ret)
		return ret;
	return status & Q3N_STATUS_ERROR ? -EIO : 0;
}

static void q3n_hw_set_page_addr(struct q3n *q3n, u32 page)
{
	u64 address = (u64)page * Q3N_PAGE_SIZE;

	q3n_hw_reg_write(q3n, Q3N_REG_ADDR_LO, (u32)address);
	q3n_hw_reg_write(q3n, Q3N_REG_ADDR_HI, (u32)(address >> 32));
}

void q3n_hw_read_window(struct q3n *q3n, void *buffer, size_t length)
{
	u8 *bytes = buffer;
	size_t offset;

	for (offset = 0; offset < length; offset += sizeof(u32)) {
		u32 value = q3n_hw_reg_read(q3n, Q3N_REG_DATA);
		size_t remaining = length - offset;
		size_t chunk = remaining < sizeof(value) ?
			       remaining : sizeof(value);

		memcpy(bytes + offset, &value, chunk);
	}
}

void q3n_hw_write_window(struct q3n *q3n, const void *buffer, size_t length)
{
	const u8 *bytes = buffer;
	size_t offset;

	for (offset = 0; offset < length; offset += sizeof(u32)) {
		u32 value;

		value = 0;
		memcpy(&value, bytes + offset, length - offset < sizeof(value) ?
		       length - offset : sizeof(value));
		q3n_hw_reg_write(q3n, Q3N_REG_DATA, value);
	}
}

u32 q3n_hw_read_capabilities(struct q3n *q3n)
{
	return q3n_hw_reg_read(q3n, Q3N_REG_CAP);
}

int q3n_hw_reset(struct q3n *q3n)
{
	if (!q3n)
		return -EINVAL;

	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_RESET);
	return q3n_hw_wait_ready(q3n);
}

int q3n_hw_read_id(struct q3n *q3n, u8 *id, size_t len)
{
	int ret;

	if (!q3n || !id || !len || len > sizeof(q3n->id))
		return -EINVAL;

	q3n_hw_reg_write(q3n, Q3N_REG_LEN, len);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_READ_ID);
	ret = q3n_hw_wait_ready(q3n);
	if (ret)
		return ret;

	q3n_hw_read_window(q3n, id, len);
	return 0;
}

int q3n_hw_read_oob(struct q3n *q3n, u32 page, void *oob)
{
	int ret;

	if (!q3n || !oob)
		return -EINVAL;

	q3n_hw_set_page_addr(q3n, page);
	q3n_hw_reg_write(q3n, Q3N_REG_OOB_LEN, Q3N_LOGICAL_OOB_SIZE);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE_OOB);
	ret = q3n_hw_wait_ready(q3n);
	if (ret)
		return ret;

	q3n_hw_read_window(q3n, oob, Q3N_LOGICAL_OOB_SIZE);
	return 0;
}

int q3n_hw_read_page(struct q3n *q3n, u32 page, void *data, void *oob,
		     bool raw, struct q3n_ecc_result *result)
{
	int ret;

	if (!q3n || !data)
		return -EINVAL;

	q3n_hw_set_page_addr(q3n, page);
	q3n_hw_reg_write(q3n, Q3N_REG_READ_FLAGS, raw ? Q3N_READ_F_RAW : 0);
	q3n_hw_reg_write(q3n, Q3N_REG_LEN, Q3N_PAGE_SIZE);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE);
	ret = q3n_hw_wait_ready(q3n);
	if (ret)
		return ret;

	q3n_hw_read_window(q3n, data, Q3N_PAGE_SIZE);
	if (!raw && result) {
		result->status = q3n_hw_reg_read(q3n, Q3N_REG_ECC_STATUS);
		result->max_bitflips =
			q3n_hw_reg_read(q3n, Q3N_REG_ECC_MAX_BITFLIPS);
		result->corrected_bits =
			q3n_hw_reg_read(q3n, Q3N_REG_ECC_CORRECTED_BITS);
		result->failed_step =
			q3n_hw_reg_read(q3n, Q3N_REG_ECC_FAILED_STEP);
	}

	if (oob)
		return q3n_hw_read_oob(q3n, page, oob);
	return 0;
}

int q3n_hw_program_oob(struct q3n *q3n, u32 page, const void *oob)
{
	if (!q3n || !oob)
		return -EINVAL;

	q3n_hw_set_page_addr(q3n, page);
	q3n_hw_reg_write(q3n, Q3N_REG_OOB_LEN, Q3N_LOGICAL_OOB_SIZE);
	q3n_hw_write_window(q3n, oob, Q3N_LOGICAL_OOB_SIZE);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_PROGRAM_PAGE_OOB);
	return q3n_hw_wait_ready(q3n);
}

int q3n_hw_program_page(struct q3n *q3n, u32 page,
			const void *data, const void *oob)
{
	int ret;

	if (!q3n || !data)
		return -EINVAL;

	q3n_hw_set_page_addr(q3n, page);
	q3n_hw_reg_write(q3n, Q3N_REG_LEN, Q3N_PAGE_SIZE);
	q3n_hw_write_window(q3n, data, Q3N_PAGE_SIZE);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_PROGRAM_PAGE);
	ret = q3n_hw_wait_ready(q3n);
	if (ret || !oob)
		return ret;

	return q3n_hw_program_oob(q3n, page, oob);
}

int q3n_hw_erase_block(struct q3n *q3n, u32 block)
{
	if (!q3n || block >= Q3N_DATA_BLOCKS)
		return -EINVAL;

	q3n_hw_set_page_addr(q3n, block * Q3N_PAGES_PER_BLOCK);
	q3n_hw_reg_write(q3n, Q3N_REG_LEN, Q3N_ERASE_SIZE);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_ERASE_BLOCK);
	return q3n_hw_wait_ready(q3n);
}

int q3n_hw_set_retry_mode(struct q3n *q3n, unsigned int mode)
{
	if (!q3n || mode >= Q3N_READ_RETRY_MODES)
		return -EINVAL;

	q3n_hw_reg_write(q3n, Q3N_REG_RETRY_MODE, mode);
	q3n->retry_mode = mode;
	return 0;
}

int q3n_hw_read_status(struct q3n *q3n, u8 *status)
{
	u32 value;

	if (!q3n || !status)
		return -EINVAL;

	value = q3n_hw_reg_read(q3n, Q3N_REG_STATUS);
	*status = Q3N_NAND_STATUS_READY | Q3N_NAND_STATUS_WP;
	if (value & Q3N_STATUS_ERROR)
		*status |= Q3N_NAND_STATUS_FAIL;
	return 0;
}
