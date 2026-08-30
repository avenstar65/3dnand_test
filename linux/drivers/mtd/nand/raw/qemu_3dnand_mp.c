// SPDX-License-Identifier: GPL-2.0

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "qemu_3dnand.h"
#include "qemu_3dnand_priv.h"

static void q3n_mp_writel(struct q3n_mp_io *io, u32 reg, u32 value)
{
	writel(value, io->regs + reg);
}

static u32 q3n_mp_readl(struct q3n_mp_io *io, u32 reg)
{
	return readl(io->regs + reg);
}

static u64 q3n_mp_phys_addr(const struct q3n_mp_io *io,
			    const struct q3n_phys_addr *addr)
{
	return ((u64)addr->block * io->pages_per_block + addr->page) *
		io->page_size;
}

static int q3n_mp_validate(const struct q3n_mp_io *io,
			   const struct q3n_mp_buffers *buffers,
			   struct q3n_mp_result *result, bool need_data)
{
	u8 plane;

	if (!io || !io->regs || !io->page_size || !io->pages_per_block ||
	    !buffers || !result || !buffers->mask ||
	    (buffers->mask & ~Q3N_MP_ALL_PLANES) || buffers->die >= Q3N_DIES)
		return -EINVAL;
	for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
		if (!(buffers->mask & BIT(plane)))
			continue;
		if (buffers->addr[plane].page >= io->pages_per_block ||
		    (need_data && !buffers->data[plane]))
			return -EINVAL;
	}
	memset(result, 0, sizeof(*result));
	return 0;
}

static void q3n_mp_select_addr(struct q3n_mp_io *io,
			       const struct q3n_mp_buffers *buffers, u8 plane)
{
	u64 addr = q3n_mp_phys_addr(io, &buffers->addr[plane]);

	q3n_mp_writel(io, Q3N_REG_MP_SLOT, plane);
	q3n_mp_writel(io, Q3N_REG_MP_ADDR_LO, lower_32_bits(addr));
	q3n_mp_writel(io, Q3N_REG_MP_ADDR_HI, upper_32_bits(addr));
}

static int q3n_mp_complete(struct q3n_mp_io *io,
			   struct q3n_mp_result *result)
{
	u32 status = q3n_mp_readl(io, Q3N_REG_STATUS);

	result->success_mask = q3n_mp_readl(io, Q3N_REG_MP_SUCCESS_MASK);
	result->failure_mask = q3n_mp_readl(io, Q3N_REG_MP_FAILURE_MASK);
	if (!(status & Q3N_STATUS_READY))
		return -ETIMEDOUT;
	if ((status & Q3N_STATUS_ERROR) || result->failure_mask)
		return -EIO;
	return 0;
}

static void q3n_mp_read_ecc(struct q3n_mp_io *io,
			    struct q3n_ecc_result *ecc)
{
	u32 status = q3n_mp_readl(io, Q3N_REG_MP_ECC_STATUS);

	ecc->max_bitflips = q3n_mp_readl(io, Q3N_REG_MP_ECC_MAX_BITFLIPS);
	ecc->corrected_bits = q3n_mp_readl(io,
					   Q3N_REG_MP_ECC_CORRECTED_BITS);
	ecc->failed_step = q3n_mp_readl(io, Q3N_REG_MP_ECC_FAILED_STEP);
	ecc->uncorrectable = status & Q3N_ECC_STATUS_UNCORRECTABLE;
}

static int q3n_mp_read_common(struct q3n_mp_io *io,
			      const struct q3n_mp_buffers *buffers,
			      struct q3n_mp_result *result, bool oob)
{
	u32 length = oob ? Q3N_LOGICAL_OOB_SIZE : io->page_size;
	u32 command = oob ? Q3N_CMD_MP_READ_PAGE_OOB : Q3N_CMD_MP_READ_PAGE;
	u8 plane;
	int ret;

	ret = q3n_mp_validate(io, buffers, result, true);
	if (ret)
		return ret;
	q3n_mp_writel(io, Q3N_REG_MP_DIE, buffers->die);
	q3n_mp_writel(io, Q3N_REG_MP_PLANE_MASK, buffers->mask);
	for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
		if (!(buffers->mask & BIT(plane)))
			continue;
		q3n_mp_select_addr(io, buffers, plane);
		if (oob)
			q3n_mp_writel(io, Q3N_REG_OOB_LEN, length);
	}
	q3n_mp_writel(io, Q3N_REG_CMD, command);
	ret = q3n_mp_complete(io, result);
	for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
		u32 offset;

		if (!(result->success_mask & BIT(plane)))
			continue;
		q3n_mp_writel(io, Q3N_REG_MP_SLOT, plane);
		for (offset = 0; offset < length; offset += sizeof(u32))
			put_unaligned_le32(q3n_mp_readl(io, Q3N_REG_DATA),
					   buffers->data[plane] + offset);
		if (!oob)
			q3n_mp_read_ecc(io, &result->ecc[plane]);
	}
	return ret;
}

static int q3n_mp_program_common(struct q3n_mp_io *io,
				 const struct q3n_mp_buffers *buffers,
				 struct q3n_mp_result *result, bool oob)
{
	u32 length = oob ? Q3N_LOGICAL_OOB_SIZE : io->page_size;
	u32 command = oob ? Q3N_CMD_MP_PROGRAM_PAGE_OOB :
		Q3N_CMD_MP_PROGRAM_PAGE;
	u8 plane;
	int ret;

	ret = q3n_mp_validate(io, buffers, result, true);
	if (ret)
		return ret;
	q3n_mp_writel(io, Q3N_REG_MP_DIE, buffers->die);
	q3n_mp_writel(io, Q3N_REG_MP_PLANE_MASK, buffers->mask);
	for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
		u32 offset;

		if (!(buffers->mask & BIT(plane)))
			continue;
		q3n_mp_select_addr(io, buffers, plane);
		q3n_mp_writel(io, oob ? Q3N_REG_OOB_LEN : Q3N_REG_LEN, length);
		for (offset = 0; offset < length; offset += sizeof(u32))
			q3n_mp_writel(io, Q3N_REG_DATA,
					 get_unaligned_le32(buffers->data[plane] +
							    offset));
	}
	q3n_mp_writel(io, Q3N_REG_CMD, command);
	return q3n_mp_complete(io, result);
}

int q3n_mp_read(struct q3n_mp_io *io, const struct q3n_mp_buffers *buffers,
		struct q3n_mp_result *result)
{
	return q3n_mp_read_common(io, buffers, result, false);
}

int q3n_mp_program(struct q3n_mp_io *io,
		   const struct q3n_mp_buffers *buffers,
		   struct q3n_mp_result *result)
{
	return q3n_mp_program_common(io, buffers, result, false);
}

int q3n_mp_read_oob(struct q3n_mp_io *io,
		    const struct q3n_mp_buffers *buffers,
		    struct q3n_mp_result *result)
{
	return q3n_mp_read_common(io, buffers, result, true);
}

int q3n_mp_program_oob(struct q3n_mp_io *io,
		       const struct q3n_mp_buffers *buffers,
		       struct q3n_mp_result *result)
{
	return q3n_mp_program_common(io, buffers, result, true);
}

int q3n_mp_erase(struct q3n_mp_io *io,
		 const struct q3n_mp_buffers *buffers,
		 struct q3n_mp_result *result)
{
	u8 plane;
	int ret;

	ret = q3n_mp_validate(io, buffers, result, false);
	if (ret)
		return ret;
	q3n_mp_writel(io, Q3N_REG_MP_DIE, buffers->die);
	q3n_mp_writel(io, Q3N_REG_MP_PLANE_MASK, buffers->mask);
	for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
		if (buffers->mask & BIT(plane))
			q3n_mp_select_addr(io, buffers, plane);
	}
	q3n_mp_writel(io, Q3N_REG_CMD, Q3N_CMD_MP_ERASE_BLOCK);
	return q3n_mp_complete(io, result);
}
