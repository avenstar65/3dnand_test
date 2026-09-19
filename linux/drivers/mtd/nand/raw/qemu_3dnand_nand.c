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

static struct qemu_3dnand *qemu_3dnand_from_chip(struct nand_chip *chip)
{
	return nand_get_controller_data(chip);
}

static int qemu_3dnand_ooblayout_ecc(struct mtd_info *mtd, int section,
				      struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;
	region->offset = mtd->oobsize;
	region->length = 0;
	return 0;
}

static int qemu_3dnand_ooblayout_free(struct mtd_info *mtd, int section,
				       struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;
	region->offset = mtd->oobsize;
	region->length = 0;
	return 0;
}

static const struct mtd_ooblayout_ops qemu_3dnand_ooblayout_ops = {
	.ecc = qemu_3dnand_ooblayout_ecc,
	.free = qemu_3dnand_ooblayout_free,
};

static int qemu_3dnand_ecc_read_oob(struct nand_chip *chip, int page)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int ret = 0;

	mutex_lock(&q3n->mtd_lock);
#if Q3N_ENABLE_MULTIPLANE_RAID
	{
		struct q3n_mp_buffers buffers;
		struct q3n_mp_result result;
		struct q3n_raid_group group;
		u8 plane;

		ret = q3n_profile_group(q3n,
						 (loff_t)page * q3n->mtd->writesize,
						 &group, NULL, NULL, NULL);
		if (ret)
			goto out;
		q3n_profile_buffers(q3n, &group, &buffers, true);
		ret = q3n_mp_read_oob(&q3n->mp, &buffers, &result);
		if (ret || result.success_mask != group.member_mask) {
			ret = ret ?: -EIO;
			goto out;
		}
		chip->oob_poi[0] = 0xff;
		for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++)
			if ((group.member_mask & BIT(plane)) &&
			    q3n->mp_oob[plane][Q3N_BBM_OOB_OFFSET] != 0xff)
				chip->oob_poi[0] = 0x00;
	}
#else
	{
		struct q3n_phys_addr phys;
		u8 oob[Q3N_MAX_LOGICAL_OOB_SIZE];

		ret = q3n_map_serial_data_page(&q3n->profile_geometry, page,
					       &phys);
		if (!ret)
			ret = q3n_hw_read_oob_locked(q3n, phys.block,
							   phys.page, oob,
							   Q3N_OP_FOREGROUND);
		if (!ret)
			memcpy(chip->oob_poi, oob, q3n->mtd->oobsize);
	}
#endif
#if Q3N_ENABLE_MULTIPLANE_RAID
out:
#endif
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static int q3n_ecc_write_oob_locked(struct nand_chip *chip, int page,
				    bool *marker_written)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int ret = 0;

#if Q3N_ENABLE_MULTIPLANE_RAID
	{
		struct q3n_mp_buffers buffers;
		struct q3n_mp_result result;
		struct q3n_raid_group group;
		u64 leb;
		u8 plane;

		ret = q3n_profile_group(q3n,
						 (loff_t)page * q3n->mtd->writesize,
						 &group, &leb, NULL, NULL);
		if (ret)
			goto out;
		q3n_profile_invalidate_leb(q3n, leb);
		q3n_profile_buffers(q3n, &group, &buffers, true);
		for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++) {
			if (!(group.member_mask & BIT(plane)))
				continue;
			memset(q3n->mp_oob[plane], 0xff, q3n->oob_size);
			q3n->mp_oob[plane][0] = chip->oob_poi[0];
		}
		ret = q3n_mp_program_oob(&q3n->mp, &buffers, &result);
		*marker_written = result.success_mask != 0;
		if (result.success_mask != group.member_mask)
			ret = -EIO;
	}
out:
#else
	{
		struct q3n_phys_addr phys;
		u8 oob[Q3N_MAX_LOGICAL_OOB_SIZE];

		memset(oob, 0xff, sizeof(oob));
		memcpy(oob, chip->oob_poi, q3n->mtd->oobsize);
		ret = q3n_map_serial_data_page(&q3n->profile_geometry, page,
					       &phys);
		if (!ret)
			ret = q3n_hw_program_oob_locked(q3n, phys.block,
							      phys.page, oob,
							      Q3N_OP_FOREGROUND);
		*marker_written = !ret;
	}
#endif
	return ret;
}

static int qemu_3dnand_ecc_write_oob(struct nand_chip *chip, int page)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	bool marker_written = false;
	int ret;

	mutex_lock(&q3n->mtd_lock);
	ret = q3n_ecc_write_oob_locked(chip, page, &marker_written);
	mutex_unlock(&q3n->mtd_lock);
	if (marker_written && chip->oob_poi[0] != 0xff && !q3n->core_markbad) {
		int bbt_ret = nand_bbt_markbad_from_oob(chip, page);

		if (bbt_ret && bbt_ret != -EOPNOTSUPP)
			return bbt_ret;
	}
	return ret;
}

static int qemu_3dnand_ecc_read_page(struct nand_chip *chip, u8 *buf,
				     int oob_required, int page)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int ret;

	mutex_lock(&q3n->mtd_lock);
#if Q3N_ENABLE_MULTIPLANE_RAID
	ret = q3n_profile_read_page_locked(q3n,
					   (loff_t)page * q3n->mtd->writesize,
					   buf);
#else
	{
		struct q3n_phys_addr phys;
		struct q3n_ecc_result ecc = {};

		ret = q3n_map_serial_data_page(&q3n->profile_geometry, page,
					       &phys);
		if (!ret)
			ret = q3n_serial_read_page_locked(q3n, phys.block,
							    phys.page, buf, NULL,
							    &ecc);
		if (!ret)
			ret = ecc.max_bitflips;
	}
#endif
	mutex_unlock(&q3n->mtd_lock);
	if (oob_required) {
		int oob_ret = qemu_3dnand_ecc_read_oob(chip, page);

		if (oob_ret && ret >= 0)
			ret = oob_ret;
	}
	if (ret == -EBADMSG) {
		q3n->mtd->ecc_stats.failed++;
		return q3n->mtd->ecc_strength;
	}
	return ret;
}

static int qemu_3dnand_ecc_write_page(struct nand_chip *chip, const u8 *buf,
				      int oob_required, int page)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int ret;

#if Q3N_ENABLE_MULTIPLANE_RAID
	mutex_lock(&q3n->mtd_lock);
	ret = q3n_profile_write_page_locked(q3n,
					    (loff_t)page * q3n->mtd->writesize,
					    buf);
	mutex_unlock(&q3n->mtd_lock);
#else
	{
		struct q3n_phys_addr phys;

		ret = q3n_map_serial_data_page(&q3n->profile_geometry, page,
					       &phys);
		if (!ret)
			ret = q3n_serial_write_page(q3n, phys.block,
							   phys.page, buf, NULL,
							   0, 0, NULL, NULL);
	}
#endif
	if (!ret && oob_required)
		ret = qemu_3dnand_ecc_write_oob(chip, page);
	return ret;
}

static int qemu_3dnand_ecc_read_page_raw(struct nand_chip *chip, u8 *buf,
					 int oob_required, int page)
{
	return -EOPNOTSUPP;
}

static int qemu_3dnand_ecc_write_page_raw(struct nand_chip *chip,
					  const u8 *buf, int oob_required,
					  int page)
{
	return -EOPNOTSUPP;
}

#if Q3N_ENABLE_MULTIPLANE_RAID
static int qemu_3dnand_block_bad(struct nand_chip *chip, loff_t ofs)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int page = div64_u64(ofs, q3n->mtd->writesize);
	int ret;

	ret = qemu_3dnand_ecc_read_oob(chip, page);
	return ret ?: chip->oob_poi[0] != 0xff;
}

static int qemu_3dnand_block_markbad(struct nand_chip *chip, loff_t ofs)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int page = div64_u64(ofs, q3n->mtd->writesize);
	int ret;

	memset(chip->oob_poi, 0xff, q3n->mtd->oobsize);
	chip->oob_poi[0] = 0x00;
	q3n->core_markbad = true;
	ret = qemu_3dnand_ecc_write_oob(chip, page);
	q3n->core_markbad = false;
	return ret;
}
#endif

static int qemu_3dnand_erase_page(struct qemu_3dnand *q3n, u32 page)
{
#if Q3N_ENABLE_MULTIPLANE_RAID
	struct q3n_mp_buffers buffers;
	struct q3n_mp_result result;
	struct q3n_raid_group group;
	u64 leb;
	int ret;

	ret = q3n_profile_group(q3n,
					 (loff_t)page * q3n->mtd->writesize,
					 &group, &leb, NULL, NULL);
	if (ret)
		return ret;
	q3n_profile_invalidate_leb(q3n, leb);
	q3n_profile_buffers(q3n, &group, &buffers, false);
	ret = q3n_mp_erase(&q3n->mp, &buffers, &result);
	if (!ret && result.success_mask != group.member_mask)
		ret = -EIO;
	return ret;
#else
	struct q3n_phys_addr phys;
	int ret;

	ret = q3n_map_serial_data_page(&q3n->profile_geometry, page, &phys);
	if (ret)
		return ret;
	ret = q3n_serial_cancel_block_parity(q3n, phys.block);
	if (ret)
		return ret;
	ret = q3n_hw_erase_block_locked(q3n, phys.block);
	if (!ret) {
		memset(&q3n->data_page_valid[phys.block * q3n->pages_per_block],
		       0, q3n->pages_per_block);
		q3n->data_meta[phys.block].generation++;
		if (!q3n->data_meta[phys.block].generation)
			q3n->data_meta[phys.block].generation = 1;
		q3n_serial_invalidate_block_parity(q3n, phys.block);
	}
	q3n_block_cancel_end(&q3n->data_meta[phys.block].parity_barrier);
	return ret;
#endif
}

static void q3n_cmd_readid_locked(struct qemu_3dnand *q3n, int column)
{
	if (column) {
		q3n->legacy.error = -EINVAL;
		return;
	}
	q3n->legacy.error = q3n_hw_read_id_locked(q3n, q3n->legacy.data,
						  Q3N_NAND_ID_LEN);
	if (!q3n->legacy.error)
		q3n->legacy.len = Q3N_NAND_ID_LEN;
}

static void q3n_cmd_erase1_locked(struct qemu_3dnand *q3n, int page_addr)
{
	if (page_addr < 0 || page_addr %
	    (q3n->mtd->erasesize / q3n->mtd->writesize)) {
		q3n->legacy.error = -EINVAL;
		return;
	}
	q3n->legacy.erase_page = page_addr;
	q3n->legacy.erase_pending = true;
}

static void q3n_cmd_erase2_locked(struct qemu_3dnand *q3n)
{
	if (!q3n->legacy.erase_pending) {
		q3n->legacy.error = -EINVAL;
		return;
	}
	q3n->legacy.erase_pending = false;
	q3n->legacy.error = qemu_3dnand_erase_page(q3n,
						   q3n->legacy.erase_page);
}

static void q3n_cmd_reset_locked(struct qemu_3dnand *q3n)
{
	q3n_hw_writel(q3n, Q3N_REG_CMD, Q3N_CMD_RESET);
	q3n->legacy.error = q3n_hw_wait_ready_locked(q3n);
	q3n->legacy.erase_pending = false;
}

static void qemu_3dnand_cmdfunc(struct nand_chip *chip, unsigned int command,
				int column, int page_addr)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);

	mutex_lock(&q3n->mtd_lock);
	q3n->legacy.len = 0;
	q3n->legacy.pos = 0;
	switch (command) {
	case NAND_CMD_READID:
		q3n_cmd_readid_locked(q3n, column);
		break;
	case NAND_CMD_STATUS:
		q3n->legacy.data[0] = NAND_STATUS_READY | NAND_STATUS_WP |
			(q3n->legacy.error ? NAND_STATUS_FAIL : 0);
		q3n->legacy.len = 1;
		break;
	case NAND_CMD_RESET:
		q3n_cmd_reset_locked(q3n);
		break;
	case NAND_CMD_ERASE1:
		q3n_cmd_erase1_locked(q3n, page_addr);
		break;
	case NAND_CMD_ERASE2:
		q3n_cmd_erase2_locked(q3n);
		break;
	case NAND_CMD_READ0:
	case NAND_CMD_READOOB:
	case NAND_CMD_SEQIN:
	case NAND_CMD_PAGEPROG:
		break;
	default:
		q3n->legacy.error = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&q3n->mtd_lock);
}

static int qemu_3dnand_waitfunc(struct nand_chip *chip)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int ret;

	mutex_lock(&q3n->mtd_lock);
	ret = q3n->legacy.error;
	q3n->legacy.error = 0;
	mutex_unlock(&q3n->mtd_lock);
	return ret ?: NAND_STATUS_READY | NAND_STATUS_WP;
}

static u8 qemu_3dnand_read_byte(struct nand_chip *chip)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);

	if (q3n->legacy.pos >= q3n->legacy.len)
		return 0xff;
	return q3n->legacy.data[q3n->legacy.pos++];
}

static void qemu_3dnand_read_buf(struct nand_chip *chip, u8 *buf, int len)
{
	while (len-- > 0)
		*buf++ = qemu_3dnand_read_byte(chip);
}

static void qemu_3dnand_write_buf(struct nand_chip *chip, const u8 *buf,
				   int len)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);

	q3n->legacy.error = -EOPNOTSUPP;
}

static void qemu_3dnand_select_chip(struct nand_chip *chip, int chipnr)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);

	if (chipnr != 0 && chipnr != -1)
		q3n->legacy.error = -EINVAL;
}

static void qemu_3dnand_sync(struct nand_chip *chip)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);

	flush_workqueue(q3n->parity_wq);
	mutex_lock(&q3n->mtd_lock);
	q3n_sched_drain(&q3n->sched);
	mutex_unlock(&q3n->mtd_lock);
}

static int qemu_3dnand_attach_chip(struct nand_chip *chip)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	int ret;

	ret = q3n_device_apply_pslc(q3n);
	if (ret)
		return ret;
	mtd_set_ooblayout(mtd, &qemu_3dnand_ooblayout_ops);
	chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
	chip->ecc.placement = NAND_ECC_PLACEMENT_OOB;
	chip->ecc.size = q3n->ecc_step_size;
	chip->ecc.strength = q3n->ecc_strength;
	chip->ecc.bytes = 0;
	chip->ecc.steps = mtd->writesize / q3n->ecc_step_size;
	chip->ecc.read_page = qemu_3dnand_ecc_read_page;
	chip->ecc.write_page = qemu_3dnand_ecc_write_page;
	chip->ecc.read_page_raw = qemu_3dnand_ecc_read_page_raw;
	chip->ecc.write_page_raw = qemu_3dnand_ecc_write_page_raw;
	chip->ecc.read_oob = qemu_3dnand_ecc_read_oob;
	chip->ecc.write_oob = qemu_3dnand_ecc_write_oob;
	chip->ecc.read_oob_raw = qemu_3dnand_ecc_read_oob;
	chip->ecc.write_oob_raw = qemu_3dnand_ecc_write_oob;
	if (!chip->ecc.steps)
		return -EINVAL;
	return 0;
}

static const struct nand_controller_ops qemu_3dnand_controller_ops = {
	.attach_chip = qemu_3dnand_attach_chip,
};

static int q3n_nand_geometry(struct qemu_3dnand *q3n, u32 *writesize,
			     u32 *erasesize, u64 *size)
{
#if Q3N_ENABLE_MULTIPLANE_RAID
	return q3n_raid_geometry_values(&q3n->profile_geometry, writesize,
					erasesize, size);
#else
	*writesize = q3n->page_size;
	*erasesize = (q3n->pages_per_block / Q3N_STRIPE_PAGES) *
		Q3N_DATA_PAGES * q3n->page_size;
	*size = (u64)q3n->data_block_count * *erasesize;
	return 0;
#endif
}

static void q3n_nand_init_chip(struct qemu_3dnand *q3n)
{
	nand_controller_init(&q3n->controller);
	q3n->controller.ops = &qemu_3dnand_controller_ops;
	q3n->chip.controller = &q3n->controller;
	nand_set_controller_data(&q3n->chip, q3n);
	q3n->chip.legacy.cmdfunc = qemu_3dnand_cmdfunc;
	q3n->chip.legacy.waitfunc = qemu_3dnand_waitfunc;
	q3n->chip.legacy.read_byte = qemu_3dnand_read_byte;
	q3n->chip.legacy.read_buf = qemu_3dnand_read_buf;
	q3n->chip.legacy.write_buf = qemu_3dnand_write_buf;
	q3n->chip.legacy.select_chip = qemu_3dnand_select_chip;
#if Q3N_ENABLE_MULTIPLANE_RAID
	q3n->chip.legacy.block_bad = qemu_3dnand_block_bad;
	q3n->chip.legacy.block_markbad = qemu_3dnand_block_markbad;
#endif
	q3n->chip.ops.sync = qemu_3dnand_sync;
}

static void q3n_nand_init_mtd(struct qemu_3dnand *q3n)
{
	q3n->mtd = nand_to_mtd(&q3n->chip);
	q3n->mtd->name = "qemu-3dnand";
	q3n->mtd->dev.parent = &q3n->pdev->dev;
	q3n->mtd->owner = THIS_MODULE;
	q3n->mtd->bitflip_threshold = q3n->ecc_strength;
	q3n->mtd->priv = q3n;
}

static int q3n_nand_init_partitions(struct qemu_3dnand *q3n)
{
	u64 test_size;
	int ret;

	ret = q3n_device_test_partition_size(q3n->mtd->size,
		q3n->mtd->erasesize, &test_size);
	if (ret)
		return ret;
	q3n->partitions[0] = (struct mtd_partition) {
		.name = "qemu-3dnand-test", .offset = 0, .size = test_size,
	};
	q3n->partitions[1] = (struct mtd_partition) {
		.name = "qemu-3dnand-data", .offset = MTDPART_OFS_APPEND,
		.size = MTDPART_SIZ_FULL,
	};
	return 0;
}

static int q3n_nand_scan_and_register(struct qemu_3dnand *q3n, u64 size)
{
	int ret;

	ret = nand_scan_with_ids(&q3n->chip, 1, q3n->scan_ids);
	if (ret)
		return ret;
	q3n->scanned = true;
	if (q3n->mtd->size != size || !nand_is_slc(&q3n->chip) ||
	    q3n->mtd->type != MTD_NANDFLASH) {
		ret = -EINVAL;
		goto err_cleanup;
	}
	ret = q3n_nand_init_partitions(q3n);
	if (ret)
		goto err_cleanup;
	ret = mtd_device_register(q3n->mtd, q3n->partitions,
		ARRAY_SIZE(q3n->partitions));
	if (ret)
		goto err_cleanup;
	return 0;

err_cleanup:
	nand_cleanup(&q3n->chip);
	q3n->scanned = false;
	return ret;
}

int q3n_nand_register(struct qemu_3dnand *q3n)
{
	u32 writesize;
	u32 erasesize;
	u64 size;
	int ret;

	ret = q3n_nand_geometry(q3n, &writesize, &erasesize, &size);
	if (ret || !writesize || !erasesize || !size || size > U64_MAX - SZ_1M)
		return ret ?: -EINVAL;
	ret = q3n_device_build_scan_id(q3n, &q3n->scan_ids[0], writesize,
		Q3N_ENABLE_MULTIPLANE_RAID ? 1 : q3n->oob_size,
		erasesize, size);
	if (ret)
		return ret;
	memset(&q3n->scan_ids[1], 0, sizeof(q3n->scan_ids[1]));
	q3n_nand_init_chip(q3n);
	q3n_nand_init_mtd(q3n);
	return q3n_nand_scan_and_register(q3n, size);
}

void q3n_nand_unregister(struct qemu_3dnand *q3n)
{
	if (q3n->mtd)
		mtd_device_unregister(q3n->mtd);
}

void q3n_nand_cleanup(struct qemu_3dnand *q3n)
{
	if (!q3n->scanned)
		return;
	nand_cleanup(&q3n->chip);
	q3n->scanned = false;
}
