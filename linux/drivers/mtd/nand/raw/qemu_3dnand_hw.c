// SPDX-License-Identifier: GPL-2.0
/* QEMU 3D NAND Linux driver — split by functional responsibility. */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/lockdep.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "qemu_3dnand_internal.h"

u32 q3n_hw_readl(struct qemu_3dnand *q3n, u32 reg)
{
	return readl(q3n->regs + reg);
}

int q3n_hw_read_id_locked(struct qemu_3dnand *q3n, u8 *id, size_t len)
{
	size_t offset;
	int ret;

	lockdep_assert_held(&q3n->mtd_lock);
	if (!id || !len || len > Q3N_NAND_ID_LEN)
		return -EINVAL;
	q3n_hw_writel(q3n, Q3N_REG_CMD, Q3N_CMD_READ_ID);
	ret = q3n_hw_wait_ready_locked(q3n);
	if (ret)
		return ret;
	for (offset = 0; offset < len; offset += sizeof(u32)) {
		__le32 value = cpu_to_le32(q3n_hw_readl(q3n, Q3N_REG_DATA));
		size_t chunk = min_t(size_t, sizeof(value), len - offset);

		memcpy(id + offset, &value, chunk);
	}
	return 0;
}

void q3n_hw_writel(struct qemu_3dnand *q3n, u32 reg, u32 val)
{
	writel(val, q3n->regs + reg);
}

static void qemu_3dnand_read_ecc_result(struct qemu_3dnand *q3n,
					struct q3n_ecc_result *ecc)
{
	u32 status;

	if (!ecc)
		return;

	status = q3n_hw_readl(q3n, Q3N_REG_ECC_STATUS);
	ecc->max_bitflips =
		q3n_hw_readl(q3n, Q3N_REG_ECC_MAX_BITFLIPS);
	ecc->corrected_bits =
		q3n_hw_readl(q3n, Q3N_REG_ECC_CORRECTED_BITS);
	ecc->failed_step = q3n_hw_readl(q3n, Q3N_REG_ECC_FAILED_STEP);
	ecc->uncorrectable = status & Q3N_ECC_STATUS_UNCORRECTABLE;
}

void
q3n_hw_account_background_ecc(struct qemu_3dnand *q3n,
				   const struct q3n_ecc_result *ecc)
{
	q3n->background_ecc_corrected_bits += ecc->corrected_bits;
}

void
q3n_hw_account_foreground_ecc(struct qemu_3dnand *q3n,
				   struct mtd_req_stats *stats,
				   const struct q3n_ecc_result *ecc)
{
	q3n->mtd->ecc_stats.corrected += ecc->corrected_bits;
	if (stats)
		stats->corrected_bitflips += ecc->corrected_bits;
}

int q3n_hw_wait_ready_locked(struct qemu_3dnand *q3n)
{
	u32 status;

	lockdep_assert_held(&q3n->mtd_lock);
	status = q3n_hw_readl(q3n, Q3N_REG_STATUS);
	if (!(status & Q3N_STATUS_READY))
		return -ETIMEDOUT;

	if (status & Q3N_STATUS_ERROR)
		return -EIO;

	return 0;
}

loff_t q3n_hw_phys_addr(struct qemu_3dnand *q3n, u32 block,
				    u32 page)
{
	return ((loff_t)block * q3n->pages_per_block + page) * q3n->page_size;
}

static void qemu_3dnand_set_addr(struct qemu_3dnand *q3n, loff_t addr)
{
	q3n_hw_writel(q3n, Q3N_REG_ADDR_LO, lower_32_bits(addr));
	q3n_hw_writel(q3n, Q3N_REG_ADDR_HI, upper_32_bits(addr));
}

int q3n_hw_read_page_locked(struct qemu_3dnand *q3n,
					     u32 block, u32 page, u8 *buf,
					     u32 op_class,
					     struct q3n_ecc_result *ecc)
{
	u32 *words = (u32 *)buf;
	int ret;
	u32 i;

	lockdep_assert_held(&q3n->mtd_lock);
	qemu_3dnand_set_addr(q3n, q3n_hw_phys_addr(q3n, block, page));
	q3n_hw_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	q3n_hw_writel(q3n, Q3N_REG_LEN, q3n->page_size);
	q3n_hw_writel(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE);
	if (ecc)
		*ecc = (struct q3n_ecc_result) {};
	ret = q3n_hw_wait_ready_locked(q3n);
	if (ret)
		return ret;

	for (i = 0; i < q3n->page_size / sizeof(u32); i++)
		words[i] = q3n_hw_readl(q3n, Q3N_REG_DATA);
	qemu_3dnand_read_ecc_result(q3n, ecc);

	return 0;
}

int q3n_hw_read_oob_locked(
		struct qemu_3dnand *q3n, u32 block, u32 page,
		u8 logical_oob[Q3N_LOGICAL_OOB_SIZE], u32 op_class)
{
	int ret;
	u32 i;

	lockdep_assert_held(&q3n->mtd_lock);
	qemu_3dnand_set_addr(q3n, q3n_hw_phys_addr(q3n, block, page));
	q3n_hw_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	q3n_hw_writel(q3n, Q3N_REG_OOB_LEN, Q3N_LOGICAL_OOB_SIZE);
	q3n_hw_writel(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE_OOB);
	ret = q3n_hw_wait_ready_locked(q3n);
	if (ret)
		return ret;

	for (i = 0; i < Q3N_LOGICAL_OOB_SIZE; i += sizeof(u32))
		put_unaligned_le32(q3n_hw_readl(q3n, Q3N_REG_DATA),
				   logical_oob + i);

	return 0;
}

int q3n_hw_program_page_locked(
		struct qemu_3dnand *q3n, u32 block, u32 page, const u8 *data,
		u32 op_class)
{
	int ret;
	u32 i;

	lockdep_assert_held(&q3n->mtd_lock);
	qemu_3dnand_set_addr(q3n, q3n_hw_phys_addr(q3n, block, page));
	q3n_hw_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	q3n_hw_writel(q3n, Q3N_REG_LEN, Q3N_PAGE_SIZE);
	for (i = 0; i < Q3N_PAGE_SIZE; i += sizeof(u32))
		q3n_hw_writel(q3n, Q3N_REG_DATA,
				     get_unaligned_le32(data + i));
	q3n_hw_writel(q3n, Q3N_REG_CMD, Q3N_CMD_PROGRAM_PAGE);
	ret = q3n_hw_wait_ready_locked(q3n);
	return ret;
}

int q3n_hw_program_oob_locked(
		struct qemu_3dnand *q3n, u32 block, u32 page,
		const u8 logical_oob[Q3N_LOGICAL_OOB_SIZE], u32 op_class)
{
	int ret;
	u32 i;

	lockdep_assert_held(&q3n->mtd_lock);
	qemu_3dnand_set_addr(q3n, q3n_hw_phys_addr(q3n, block, page));
	q3n_hw_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	q3n_hw_writel(q3n, Q3N_REG_OOB_LEN, Q3N_LOGICAL_OOB_SIZE);
	for (i = 0; i < Q3N_LOGICAL_OOB_SIZE; i += sizeof(u32))
		q3n_hw_writel(q3n, Q3N_REG_DATA,
				     get_unaligned_le32(logical_oob + i));

	q3n_hw_writel(q3n, Q3N_REG_CMD, Q3N_CMD_PROGRAM_PAGE_OOB);
	ret = q3n_hw_wait_ready_locked(q3n);
	return ret;
}

int q3n_hw_erase_block_locked(struct qemu_3dnand *q3n,
					       u32 block)
{
	lockdep_assert_held(&q3n->mtd_lock);
	qemu_3dnand_set_addr(q3n, q3n_hw_phys_addr(q3n, block, 0));
	q3n_hw_writel(q3n, Q3N_REG_LEN,
			   q3n->pages_per_block * q3n->page_size);
	q3n_hw_writel(q3n, Q3N_REG_CMD, Q3N_CMD_ERASE_BLOCK);
	return q3n_hw_wait_ready_locked(q3n);
}

void q3n_hw_build_bad_block_oob(u8 *logical_oob)
{
	memset(logical_oob, 0xff, Q3N_LOGICAL_OOB_SIZE);
	logical_oob[0] = 0x00;
}

int q3n_hw_get_block_status_locked(
		struct qemu_3dnand *q3n, u32 block, u32 *status)
{
	int ret;

	lockdep_assert_held(&q3n->mtd_lock);
	if (block >= q3n->data_block_count || !status)
		return -EINVAL;

	qemu_3dnand_set_addr(q3n, q3n_hw_phys_addr(q3n, block, 0));
	q3n_hw_writel(q3n, Q3N_REG_CMD, Q3N_CMD_GET_BLOCK_STATUS);
	ret = q3n_hw_wait_ready_locked(q3n);
	if (ret)
		return ret;

	*status = q3n_hw_readl(q3n, Q3N_REG_BLOCK_STATUS);
	return 0;
}
