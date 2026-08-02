// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#else
#include <linux/errno.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#endif

#include "qemu_3dnand_ecc.h"
#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_multiplane_layout.h"
#include "qemu_3dnand_page.h"
#include "qemu_3dnand_regs.h"

int q3n_ecc_account_page_result(const struct q3n_page_result *result,
				unsigned int retry_mode,
				struct q3n_ecc_stats *stats)
{
	u32 bitflips;

	if (!result || !stats)
		return -EINVAL;

	if (result->failed_plane_mask || result->failed_data_pages) {
		stats->failed++;
		return Q3N_ECC_STRENGTH;
	}

	stats->corrected += result->corrected_bits;
	bitflips = result->max_bitflips;
	if (retry_mode && bitflips < Q3N_ECC_STRENGTH)
		bitflips = Q3N_ECC_STRENGTH;

	return bitflips;
}

#ifndef Q3N_HOST_TEST
static int q3n_ooblayout_ecc(struct mtd_info *mtd, int section,
			     struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	region->offset = mtd->oobsize;
	region->length = 0;
	return 0;
}

static int q3n_ooblayout_free(struct mtd_info *mtd, int section,
			      struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	region->offset = 1;
	region->length = mtd->oobsize - 1;
	return 0;
}

static const struct mtd_ooblayout_ops q3n_ooblayout_ops = {
	.ecc = q3n_ooblayout_ecc,
	.free = q3n_ooblayout_free,
};

static int q3n_multiplane_ooblayout_free(struct mtd_info *mtd, int section,
					 struct mtd_oob_region *region)
{
	u32 offset;
	u32 length;
	int ret;

	ret = q3n_multiplane_oob_free_region(section, &offset, &length);
	if (ret)
		return ret;
	if (offset + length > mtd->oobsize)
		return -ERANGE;

	region->offset = offset;
	region->length = length;
	return 0;
}

static const struct mtd_ooblayout_ops q3n_multiplane_ooblayout_ops = {
	.ecc = q3n_ooblayout_ecc,
	.free = q3n_multiplane_ooblayout_free,
};

int q3n_ecc_read_page(struct nand_chip *chip, u8 *buf,
		      int oob_required, int page)
{
	struct q3n *q3n = nand_to_q3n(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct q3n_page_result result = { };
	struct q3n_ecc_stats stats = {
		.corrected = mtd->ecc_stats.corrected,
		.failed = mtd->ecc_stats.failed,
	};
	int ret;

	mutex_lock(&q3n->lock);
	ret = q3n_page_read(q3n, page, buf,
			    oob_required ? chip->oob_poi : NULL,
			    false, &result);
	mutex_unlock(&q3n->lock);
	if (ret)
		return ret;

	ret = q3n_ecc_account_page_result(&result, q3n->retry_mode, &stats);
	mtd->ecc_stats.corrected = stats.corrected;
	mtd->ecc_stats.failed = stats.failed;
	return ret;
}

int q3n_ecc_write_page(struct nand_chip *chip, const u8 *buf,
		       int oob_required, int page)
{
	struct q3n *q3n = nand_to_q3n(chip);
	int ret;

	mutex_lock(&q3n->lock);
	ret = q3n_page_write(q3n, page, buf,
			     oob_required ? chip->oob_poi : NULL);
	mutex_unlock(&q3n->lock);
	return ret;
}

int q3n_ecc_read_page_raw(struct nand_chip *chip, u8 *buf,
			  int oob_required, int page)
{
	struct q3n *q3n = nand_to_q3n(chip);
	int ret;

	mutex_lock(&q3n->lock);
	ret = q3n_page_read(q3n, page, buf,
			    oob_required ? chip->oob_poi : NULL,
			    true, NULL);
	mutex_unlock(&q3n->lock);
	return ret;
}

int q3n_ecc_write_page_raw(struct nand_chip *chip, const u8 *buf,
			   int oob_required, int page)
{
	return q3n_ecc_write_page(chip, buf, oob_required, page);
}

int q3n_ecc_read_oob(struct nand_chip *chip, int page)
{
	struct q3n *q3n = nand_to_q3n(chip);
	int ret;

	mutex_lock(&q3n->lock);
	ret = q3n_page_read_oob(q3n, page, chip->oob_poi);
	mutex_unlock(&q3n->lock);
	return ret;
}

int q3n_ecc_write_oob(struct nand_chip *chip, int page)
{
	struct q3n *q3n = nand_to_q3n(chip);
	int ret;

	mutex_lock(&q3n->lock);
	ret = q3n_page_write_oob(q3n, page, chip->oob_poi);
	mutex_unlock(&q3n->lock);
	return ret;
}

int q3n_setup_read_retry(struct nand_chip *chip, int retry_mode)
{
	struct q3n *q3n = nand_to_q3n(chip);
	int ret;

	mutex_lock(&q3n->lock);
	ret = q3n_hw_set_retry_mode(q3n, retry_mode);
	mutex_unlock(&q3n->lock);
	return ret;
}

int q3n_ecc_init(struct q3n *q3n)
{
	struct nand_chip *chip;
	struct mtd_info *mtd;
	u64 erasesize;

	if (!q3n)
		return -EINVAL;

	chip = &q3n->chip;
	mtd = nand_to_mtd(chip);
	erasesize = (u64)q3n->geometry.writesize *
		q3n->geometry.pages_per_block;
	if (mtd->writesize != q3n->geometry.writesize ||
	    mtd->oobsize != q3n->geometry.oobsize ||
	    mtd->erasesize != erasesize ||
	    mtd->size != q3n->logical_size ||
	    mtd->writesize % Q3N_ECC_STEP_SIZE)
		return -EINVAL;

	if (q3n->storage_mode == Q3N_MODE_MULTIPLANE)
		mtd_set_ooblayout(mtd, &q3n_multiplane_ooblayout_ops);
	else
		mtd_set_ooblayout(mtd, &q3n_ooblayout_ops);

	chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
	chip->ecc.placement = NAND_ECC_PLACEMENT_OOB;
	chip->ecc.size = Q3N_ECC_STEP_SIZE;
	chip->ecc.strength = Q3N_ECC_STRENGTH;
	chip->ecc.bytes = 0;
	chip->ecc.steps = mtd->writesize / Q3N_ECC_STEP_SIZE;
	chip->ecc.read_page = q3n_ecc_read_page;
	chip->ecc.write_page = q3n_ecc_write_page;
	chip->ecc.read_page_raw = q3n_ecc_read_page_raw;
	chip->ecc.write_page_raw = q3n_ecc_write_page_raw;
	chip->ecc.read_oob = q3n_ecc_read_oob;
	chip->ecc.write_oob = q3n_ecc_write_oob;
	chip->ecc.read_oob_raw = q3n_ecc_read_oob;
	chip->ecc.write_oob_raw = q3n_ecc_write_oob;
	chip->read_retries = Q3N_READ_RETRY_MODES;
	chip->ops.setup_read_retry = q3n_setup_read_retry;
	return 0;
}
#endif
