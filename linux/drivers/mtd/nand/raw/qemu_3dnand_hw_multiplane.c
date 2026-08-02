// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#include <string.h>
#else
#include <linux/errno.h>
#include <linux/string.h>
#endif

#include "qemu_3dnand_hw_multiplane.h"
#include "qemu_3dnand_regs.h"

#define Q3N_MP_MASK_ALL ((1U << Q3N_MP_PLANES) - 1U)
#define Q3N_MP_MAIN_SIZE (Q3N_PAGE_SIZE * Q3N_MP_PLANES)
#define Q3N_MP_OOB_SIZE (Q3N_LOGICAL_OOB_SIZE * Q3N_MP_PLANES)

static void q3n_hw_mp_set_addr(struct q3n *q3n,
				const struct q3n_mp_addr *addr)
{
	q3n_hw_reg_write(q3n, Q3N_REG_MP_DIE, addr->die);
	q3n_hw_reg_write(q3n, Q3N_REG_MP_BLOCK, addr->block_in_plane);
	q3n_hw_reg_write(q3n, Q3N_REG_MP_PAGE, addr->page_in_block);
}

static void q3n_hw_mp_clear_result(struct q3n_mp_result *result)
{
	memset(result, 0, sizeof(*result));
}

static int q3n_hw_mp_finish(struct q3n *q3n, struct q3n_mp_result *result)
{
	u32 status;
	u32 done;
	u32 fail;
	u32 plane;
	int ret;

	ret = q3n_hw_wait_ready_status(q3n, &status);
	if (ret)
		return ret;

	done = q3n_hw_reg_read(q3n, Q3N_REG_MP_DONE_MASK);
	fail = q3n_hw_reg_read(q3n, Q3N_REG_MP_FAIL_MASK);
	result->done_mask = done;
	result->fail_mask = fail;

	if ((done | fail) == 0 && !(done & fail))
		return status & Q3N_STATUS_ERROR ? -EIO : -EINVAL;
	if ((done & ~Q3N_MP_MASK_ALL) || (fail & ~Q3N_MP_MASK_ALL) ||
	    (done | fail) != Q3N_MP_MASK_ALL || (done & fail))
		return -EPROTO;

	for (plane = 0; plane < Q3N_MP_PLANES; plane++)
		if (fail & BIT(plane))
			result->plane[plane].status = -EIO;

	return (status & Q3N_STATUS_ERROR) || fail ? -EIO : 0;
}

static void q3n_hw_mp_read_ecc(struct q3n *q3n,
				struct q3n_mp_result *result)
{
	u32 plane;

	for (plane = 0; plane < Q3N_MP_PLANES; plane++) {
		struct q3n_ecc_result *ecc = &result->plane[plane].ecc;

		q3n_hw_reg_write(q3n, Q3N_REG_MP_ECC_SELECT, plane);
		ecc->status = q3n_hw_reg_read(q3n, Q3N_REG_MP_ECC_STATUS);
		ecc->max_bitflips = q3n_hw_reg_read(q3n,
						 Q3N_REG_MP_ECC_MAX_BITFLIPS);
		ecc->corrected_bits = q3n_hw_reg_read(q3n,
						   Q3N_REG_MP_ECC_CORRECTED_BITS);
		ecc->failed_step = q3n_hw_reg_read(q3n,
						 Q3N_REG_MP_ECC_FAILED_STEP);
	}
}

static struct q3n_mp_result *q3n_hw_mp_result_or_local(
		struct q3n_mp_result *result, struct q3n_mp_result *local)
{
	if (result)
		return result;
	return local;
}

int q3n_hw_mp_read_page(struct q3n *q3n, const struct q3n_mp_addr *addr,
			void *data, bool raw, struct q3n_mp_result *result)
{
	struct q3n_mp_result local;
	struct q3n_mp_result *transport = q3n_hw_mp_result_or_local(result, &local);
	int ret;

	q3n_hw_mp_clear_result(transport);
	if (!q3n || !addr || !data)
		return -EINVAL;

	q3n_hw_mp_set_addr(q3n, addr);
	q3n_hw_reg_write(q3n, Q3N_REG_READ_FLAGS, raw ? Q3N_READ_F_RAW : 0);
	q3n_hw_reg_write(q3n, Q3N_REG_LEN, Q3N_MP_MAIN_SIZE);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_MP_READ_PAGE);
	ret = q3n_hw_mp_finish(q3n, transport);
	if (ret == -EPROTO || ret == -EINVAL || ret == -ETIMEDOUT)
		return ret;
	if (!raw)
		q3n_hw_mp_read_ecc(q3n, transport);
	q3n_hw_read_window(q3n, data, Q3N_MP_MAIN_SIZE);
	return ret;
}

int q3n_hw_mp_program_page(struct q3n *q3n, const struct q3n_mp_addr *addr,
			   const void *data, struct q3n_mp_result *result)
{
	struct q3n_mp_result local;
	struct q3n_mp_result *transport = q3n_hw_mp_result_or_local(result, &local);

	q3n_hw_mp_clear_result(transport);
	if (!q3n || !addr || !data)
		return -EINVAL;

	q3n_hw_mp_set_addr(q3n, addr);
	q3n_hw_reg_write(q3n, Q3N_REG_LEN, Q3N_MP_MAIN_SIZE);
	q3n_hw_write_window(q3n, data, Q3N_MP_MAIN_SIZE);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_MP_PROGRAM_PAGE);
	return q3n_hw_mp_finish(q3n, transport);
}

int q3n_hw_mp_read_oob(struct q3n *q3n, const struct q3n_mp_addr *addr,
		       void *oob, struct q3n_mp_result *result)
{
	struct q3n_mp_result local;
	struct q3n_mp_result *transport = q3n_hw_mp_result_or_local(result, &local);
	int ret;

	q3n_hw_mp_clear_result(transport);
	if (!q3n || !addr || !oob)
		return -EINVAL;

	q3n_hw_mp_set_addr(q3n, addr);
	q3n_hw_reg_write(q3n, Q3N_REG_OOB_LEN, Q3N_MP_OOB_SIZE);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_MP_READ_OOB);
	ret = q3n_hw_mp_finish(q3n, transport);
	if (ret == -EPROTO || ret == -EINVAL || ret == -ETIMEDOUT)
		return ret;
	q3n_hw_read_window(q3n, oob, Q3N_MP_OOB_SIZE);
	return ret;
}

int q3n_hw_mp_program_oob(struct q3n *q3n, const struct q3n_mp_addr *addr,
			  const void *oob, struct q3n_mp_result *result)
{
	struct q3n_mp_result local;
	struct q3n_mp_result *transport = q3n_hw_mp_result_or_local(result, &local);

	q3n_hw_mp_clear_result(transport);
	if (!q3n || !addr || !oob)
		return -EINVAL;

	q3n_hw_mp_set_addr(q3n, addr);
	q3n_hw_reg_write(q3n, Q3N_REG_OOB_LEN, Q3N_MP_OOB_SIZE);
	q3n_hw_write_window(q3n, oob, Q3N_MP_OOB_SIZE);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_MP_PROGRAM_OOB);
	return q3n_hw_mp_finish(q3n, transport);
}

int q3n_hw_mp_erase_group(struct q3n *q3n, const struct q3n_mp_addr *addr,
			  struct q3n_mp_result *result)
{
	struct q3n_mp_result local;
	struct q3n_mp_result *transport = q3n_hw_mp_result_or_local(result, &local);

	q3n_hw_mp_clear_result(transport);
	if (!q3n || !addr)
		return -EINVAL;

	q3n_hw_mp_set_addr(q3n, addr);
	q3n_hw_reg_write(q3n, Q3N_REG_CMD, Q3N_CMD_MP_ERASE_GROUP);
	return q3n_hw_mp_finish(q3n, transport);
}
