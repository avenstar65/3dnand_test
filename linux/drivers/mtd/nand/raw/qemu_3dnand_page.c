// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#else
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#endif

#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_page.h"
#include "qemu_3dnand_regs.h"

#ifdef CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID
#include "qemu_3dnand_page_raid.h"
#endif

bool q3n_page_buffer_erased(const void *buffer, size_t length)
{
	const u8 *bytes = buffer;
	size_t i;

	if (!buffer)
		return true;

	for (i = 0; i < length; i++) {
		if (bytes[i] != 0xff)
			return false;
	}
	return true;
}

static void q3n_page_result_from_ecc(struct q3n_page_result *result,
				     const struct q3n_ecc_result *ecc)
{
	if (!result)
		return;

	memset(result, 0, sizeof(*result));
	result->max_bitflips = ecc->max_bitflips;
	result->corrected_bits = ecc->corrected_bits;
	if (ecc->status & Q3N_ECC_STATUS_UNCORRECTABLE)
		result->failed_data_pages = 1;
}

static int q3n_identity_read_page(struct q3n *q3n, u32 logical_page,
				  void *data, void *oob, bool raw,
				  struct q3n_page_result *result)
{
	struct q3n_ecc_result ecc = { 0 };
	int ret;

	ret = q3n_hw_read_page(q3n, logical_page, data, oob, raw, &ecc);
	if (ret)
		return ret;

	if (raw) {
		if (result)
			memset(result, 0, sizeof(*result));
	} else {
		q3n_page_result_from_ecc(result, &ecc);
	}
	return 0;
}

static int q3n_identity_write_page(struct q3n *q3n, u32 logical_page,
				   const void *data, const void *oob)
{
	if (!q3n_page_buffer_erased(data, Q3N_PAGE_SIZE))
		return q3n_hw_program_page(q3n, logical_page, data, oob);
	if (q3n_page_buffer_erased(oob, Q3N_LOGICAL_OOB_SIZE))
		return 0;
	return q3n_hw_program_oob(q3n, logical_page, oob);
}

static int q3n_identity_read_oob(struct q3n *q3n, u32 logical_page, void *oob)
{
	return q3n_hw_read_oob(q3n, logical_page, oob);
}

static int q3n_identity_write_oob(struct q3n *q3n, u32 logical_page,
				  const void *oob)
{
	if (q3n_page_buffer_erased(oob, Q3N_LOGICAL_OOB_SIZE))
		return 0;
	return q3n_hw_program_oob(q3n, logical_page, oob);
}

static int q3n_identity_erase_block(struct q3n *q3n, u32 logical_block)
{
	return q3n_hw_erase_block(q3n, logical_block);
}

static const struct q3n_page_ops q3n_identity_page_ops = {
	.read_page = q3n_identity_read_page,
	.write_page = q3n_identity_write_page,
	.read_oob = q3n_identity_read_oob,
	.write_oob = q3n_identity_write_oob,
	.erase_block = q3n_identity_erase_block,
};

#ifdef CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID
static void *q3n_page_scratch_alloc(void)
{
#ifdef Q3N_HOST_TEST
	return malloc(Q3N_PAGE_SIZE);
#else
	return kmalloc(Q3N_PAGE_SIZE, GFP_KERNEL);
#endif
}
#endif

static void q3n_page_scratch_free(void *scratch)
{
#ifdef Q3N_HOST_TEST
	free(scratch);
#else
	kfree(scratch);
#endif
}

int q3n_page_layer_init(struct q3n *q3n, bool raid_enabled, u32 data_pages)
{
	struct q3n_page_profile profile;
	int ret;

	if (!q3n)
		return -EINVAL;

	ret = q3n_layout_build(&profile, &q3n->physical_geometry,
			       raid_enabled, data_pages);
	if (ret)
		return ret;

	q3n->page_profile = profile;
	q3n->geometry = profile.logical;
	q3n->parity_scratch = NULL;
	q3n->raid_recovered_pages = 0;

	if (!raid_enabled) {
		q3n->page_ops = &q3n_identity_page_ops;
		return 0;
	}

#ifndef CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID
	q3n->page_ops = NULL;
	return -EOPNOTSUPP;
#else
	q3n->parity_scratch = q3n_page_scratch_alloc();
	if (!q3n->parity_scratch) {
		q3n->page_ops = NULL;
		return -ENOMEM;
	}
	q3n->page_ops = q3n_page_raid_get_ops();
	return 0;
#endif
}

void q3n_page_layer_cleanup(struct q3n *q3n)
{
	if (!q3n)
		return;

	q3n_page_scratch_free(q3n->parity_scratch);
	q3n->parity_scratch = NULL;
	q3n->page_ops = NULL;
}

static int q3n_page_in_range(const struct q3n *q3n, u32 logical_page)
{
	u64 pages;

	pages = (u64)q3n->geometry.pages_per_block * q3n->geometry.blocks;
	return (u64)logical_page < pages;
}

int q3n_page_read(struct q3n *q3n, u32 logical_page,
		  void *data, void *oob, bool raw,
		  struct q3n_page_result *result)
{
	if (!q3n || !q3n->page_ops || !data ||
	    !q3n_page_in_range(q3n, logical_page))
		return -EINVAL;

	return q3n->page_ops->read_page(q3n, logical_page, data, oob, raw,
					result);
}

int q3n_page_write(struct q3n *q3n, u32 logical_page,
		   const void *data, const void *oob)
{
	if (!q3n || !q3n->page_ops || !data ||
	    !q3n_page_in_range(q3n, logical_page))
		return -EINVAL;

	return q3n->page_ops->write_page(q3n, logical_page, data, oob);
}

int q3n_page_read_oob(struct q3n *q3n, u32 logical_page, void *oob)
{
	if (!q3n || !q3n->page_ops || !oob ||
	    !q3n_page_in_range(q3n, logical_page))
		return -EINVAL;

	return q3n->page_ops->read_oob(q3n, logical_page, oob);
}

int q3n_page_write_oob(struct q3n *q3n, u32 logical_page, const void *oob)
{
	if (!q3n || !q3n->page_ops || !oob ||
	    !q3n_page_in_range(q3n, logical_page))
		return -EINVAL;

	return q3n->page_ops->write_oob(q3n, logical_page, oob);
}

int q3n_page_erase_block(struct q3n *q3n, u32 logical_block)
{
	if (!q3n || !q3n->page_ops || logical_block >= q3n->geometry.blocks)
		return -EINVAL;

	return q3n->page_ops->erase_block(q3n, logical_block);
}
