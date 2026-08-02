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

#ifdef CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE
#include "qemu_3dnand_multiplane.h"
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

#if defined(CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID) || \
	defined(CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE)
static void *q3n_page_scratch_alloc(size_t length)
{
#ifdef Q3N_HOST_TEST
	return malloc(length);
#else
	return kmalloc(length, GFP_KERNEL);
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

int q3n_page_layer_init(struct q3n *q3n, enum q3n_storage_mode mode,
			u32 raid_data_pages,
			const struct q3n_flash_topology *topology)
{
	struct q3n_page_profile profile;
	int ret;

	if (!q3n)
		return -EINVAL;

	q3n_page_layer_cleanup(q3n);
	q3n->storage_mode = Q3N_MODE_IDENTITY;
	q3n->logical_size = 0;
	q3n->raid_recovered_pages = 0;
	(void)raid_data_pages;
	(void)topology;

	if (mode == Q3N_MODE_IDENTITY) {
		ret = q3n_layout_build(&profile, &q3n->physical_geometry, false, 1);
		if (ret)
			goto err_clear;
		q3n->page_profile = profile;
		q3n->geometry = profile.logical;
		q3n->logical_size = profile.logical_size;
		q3n->storage_mode = mode;
		q3n->page_ops = &q3n_identity_page_ops;
		return 0;
	}

	if (mode == Q3N_MODE_PAGE_RAID) {
#ifndef CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID
		ret = -EOPNOTSUPP;
		goto err_clear;
#else
		ret = q3n_layout_build(&profile, &q3n->physical_geometry, true,
				       raid_data_pages);
		if (ret)
			goto err_clear;
		q3n->parity_scratch = q3n_page_scratch_alloc(Q3N_PAGE_SIZE);
		if (!q3n->parity_scratch) {
			ret = -ENOMEM;
			goto err_clear;
		}
		q3n->page_profile = profile;
		q3n->geometry = profile.logical;
		q3n->logical_size = profile.logical_size;
		q3n->storage_mode = mode;
		q3n->page_ops = q3n_page_raid_get_ops();
		return 0;
#endif
	}

	if (mode != Q3N_MODE_MULTIPLANE) {
		ret = -EINVAL;
		goto err_clear;
	}
#ifndef CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE
	ret = -EOPNOTSUPP;
	goto err_clear;
#else
	struct q3n_multiplane_profile multiplane_profile;

	ret = q3n_multiplane_layout_build(&multiplane_profile,
					&q3n->physical_geometry, topology);
	if (ret)
		goto err_clear;
	q3n->multiplane_oob_scratch = q3n_page_scratch_alloc(4096);
	if (!q3n->multiplane_oob_scratch) {
		ret = -ENOMEM;
		goto err_clear;
	}
	q3n->multiplane_profile = multiplane_profile;
	q3n->topology = *topology;
	q3n->geometry = multiplane_profile.logical;
	q3n->logical_size = multiplane_profile.logical_size;
	q3n->storage_mode = mode;
	q3n->page_ops = q3n_multiplane_get_ops();
	return 0;
#endif

err_clear:
	q3n_page_layer_cleanup(q3n);
	return ret;
}

void q3n_page_layer_cleanup(struct q3n *q3n)
{
	if (!q3n)
		return;

	q3n_page_scratch_free(q3n->parity_scratch);
	q3n_page_scratch_free(q3n->multiplane_oob_scratch);
	q3n->parity_scratch = NULL;
	q3n->multiplane_oob_scratch = NULL;
	q3n->page_ops = NULL;
	q3n->storage_mode = Q3N_MODE_IDENTITY;
	q3n->logical_size = 0;
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
