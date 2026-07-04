// SPDX-License-Identifier: GPL-2.0
/*
 * Linux MTD driver for the QEMU 3D NAND controller model.
 *
 * QEMU provides only physical flash/controller semantics. This driver owns the
 * scheme-D page-raid layout: 8 data lanes plus append-only parity pages in a
 * parity block pool.
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mtd/mtd.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "qemu_3dnand.h"

#define Q3N_RAID_LANES 8

struct qemu_3dnand_data_block_meta {
	u32 generation;
	bool bad;
	bool erased;
};

struct qemu_3dnand_parity_entry {
	u32 physical_block;
	u32 page;
	u32 data_block_generation[Q3N_RAID_LANES];
	u32 parity_version;
	u64 sequence;
	bool valid;
};

struct qemu_3dnand {
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_size;
	struct mutex lock;
	struct dentry *debugfs_dir;
	struct mtd_info mtd;

	u8 *page_buf;
	u8 *raid_buf;
	u8 *data_page_valid;
	struct qemu_3dnand_data_block_meta *data_meta;
	struct qemu_3dnand_parity_entry *parity_index;

	u32 page_size;
	u32 oob_size;
	u32 pages_per_block;
	u32 blocks_per_plane;
	u32 data_blocks_per_plane;
	u32 parity_blocks_per_plane;
	u32 metadata_blocks_per_plane;
	u32 reserve_blocks_per_plane;
	u32 data_block_count;
	u32 parity_block_count;
	u32 raid_group_count;
	u64 parity_next_page;
	u64 parity_sequence;

	u64 parity_written;
	u64 parity_stale;
	u64 raid_recovered;
	u64 raid_failed;
	u64 generation_updates;
};

static u32 qemu_3dnand_readl(struct qemu_3dnand *q3n, u32 reg)
{
	return readl(q3n->regs + reg);
}

static void qemu_3dnand_writel(struct qemu_3dnand *q3n, u32 reg, u32 val)
{
	writel(val, q3n->regs + reg);
}

static int qemu_3dnand_wait_ready(struct qemu_3dnand *q3n)
{
	u32 status = qemu_3dnand_readl(q3n, Q3N_REG_STATUS);

	if (!(status & Q3N_STATUS_READY))
		return -ETIMEDOUT;

	if (status & Q3N_STATUS_ERROR)
		return -EIO;

	return 0;
}

static loff_t qemu_3dnand_phys_addr(struct qemu_3dnand *q3n, u32 block,
				    u32 page)
{
	return ((loff_t)block * q3n->pages_per_block + page) * q3n->page_size;
}

static void qemu_3dnand_set_addr(struct qemu_3dnand *q3n, loff_t addr)
{
	qemu_3dnand_writel(q3n, Q3N_REG_ADDR_LO, lower_32_bits(addr));
	qemu_3dnand_writel(q3n, Q3N_REG_ADDR_HI, upper_32_bits(addr));
}

static int qemu_3dnand_read_phys_page_locked(struct qemu_3dnand *q3n,
					     u32 block, u32 page, u8 *buf)
{
	u32 *words = (u32 *)buf;
	int ret;
	u32 i;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, page));
	qemu_3dnand_writel(q3n, Q3N_REG_LEN, q3n->page_size);
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE);
	ret = qemu_3dnand_wait_ready(q3n);
	if (ret)
		return ret;

	for (i = 0; i < q3n->page_size / sizeof(u32); i++)
		words[i] = qemu_3dnand_readl(q3n, Q3N_REG_DATA);

	return 0;
}

static int qemu_3dnand_program_phys_page_locked(struct qemu_3dnand *q3n,
						u32 block, u32 page,
						const u8 *buf)
{
	const u32 *words = (const u32 *)buf;
	u32 i;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, page));
	qemu_3dnand_writel(q3n, Q3N_REG_LEN, q3n->page_size);
	for (i = 0; i < q3n->page_size / sizeof(u32); i++)
		qemu_3dnand_writel(q3n, Q3N_REG_DATA, words[i]);

	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_PROGRAM_PAGE);
	return qemu_3dnand_wait_ready(q3n);
}

static int qemu_3dnand_erase_phys_block_locked(struct qemu_3dnand *q3n,
					       u32 block)
{
	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, 0));
	qemu_3dnand_writel(q3n, Q3N_REG_LEN,
			   q3n->pages_per_block * q3n->page_size);
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_ERASE_BLOCK);
	return qemu_3dnand_wait_ready(q3n);
}

static void qemu_3dnand_decode_logical(struct qemu_3dnand *q3n, loff_t addr,
				       u32 *data_block, u32 *page,
				       u32 *column)
{
	u64 logical_page = addr / q3n->page_size;

	*column = addr % q3n->page_size;
	*data_block = div_u64(logical_page, q3n->pages_per_block);
	*page = do_div(logical_page, q3n->pages_per_block);
}

static u32 qemu_3dnand_data_page_index(struct qemu_3dnand *q3n,
				       u32 data_block, u32 page)
{
	return data_block * q3n->pages_per_block + page;
}

static u32 qemu_3dnand_parity_index(struct qemu_3dnand *q3n, u32 group,
				    u32 page)
{
	return group * q3n->pages_per_block + page;
}

static bool qemu_3dnand_stripe_full(struct qemu_3dnand *q3n, u32 group,
				    u32 page)
{
	u32 base = group * Q3N_RAID_LANES;
	u32 lane;

	if (base + Q3N_RAID_LANES > q3n->data_block_count)
		return false;

	for (lane = 0; lane < Q3N_RAID_LANES; lane++) {
		if (!q3n->data_page_valid[qemu_3dnand_data_page_index(q3n,
								       base + lane,
								       page)])
			return false;
	}

	return true;
}

static bool qemu_3dnand_parity_generation_valid(struct qemu_3dnand *q3n,
						struct qemu_3dnand_parity_entry *entry,
						u32 group)
{
	u32 base = group * Q3N_RAID_LANES;
	u32 lane;

	if (!entry->valid || base + Q3N_RAID_LANES > q3n->data_block_count)
		return false;

	for (lane = 0; lane < Q3N_RAID_LANES; lane++) {
		if (entry->data_block_generation[lane] !=
		    q3n->data_meta[base + lane].generation)
			return false;
	}

	return true;
}

static void qemu_3dnand_mark_parity_stale(struct qemu_3dnand *q3n,
					  struct qemu_3dnand_parity_entry *entry)
{
	if (!entry->valid)
		return;

	entry->valid = false;
	q3n->parity_stale++;
}

static int qemu_3dnand_append_parity_locked(struct qemu_3dnand *q3n,
					    u32 group, u32 page)
{
	struct qemu_3dnand_parity_entry *entry;
	u64 parity_capacity = (u64)q3n->parity_block_count * q3n->pages_per_block;
	u64 parity_slot;
	u32 base = group * Q3N_RAID_LANES;
	u32 parity_block;
	u32 parity_page;
	u32 parity_version;
	u32 lane;
	u32 i;
	int ret;

	if (!qemu_3dnand_stripe_full(q3n, group, page))
		return 0;
	if (q3n->parity_next_page >= parity_capacity)
		return -ENOSPC;

	entry = &q3n->parity_index[qemu_3dnand_parity_index(q3n, group, page)];
	parity_version = entry->parity_version + 1;

	memset(q3n->raid_buf, 0, q3n->page_size);
	for (lane = 0; lane < Q3N_RAID_LANES; lane++) {
		ret = qemu_3dnand_read_phys_page_locked(q3n, base + lane, page,
							q3n->page_buf);
		if (ret)
			return ret;
		for (i = 0; i < q3n->page_size; i++)
			q3n->raid_buf[i] ^= q3n->page_buf[i];
	}

	parity_slot = q3n->parity_next_page;
	parity_page = do_div(parity_slot, q3n->pages_per_block);
	parity_block = q3n->data_block_count + parity_slot;
	ret = qemu_3dnand_program_phys_page_locked(q3n, parity_block,
						   parity_page, q3n->raid_buf);
	if (ret)
		return ret;

	entry->physical_block = parity_block;
	entry->page = parity_page;
	for (lane = 0; lane < Q3N_RAID_LANES; lane++)
		entry->data_block_generation[lane] =
			q3n->data_meta[base + lane].generation;
	entry->parity_version = parity_version;
	entry->sequence = ++q3n->parity_sequence;
	entry->valid = true;
	q3n->parity_next_page++;
	q3n->parity_written++;
	return 0;
}

static int qemu_3dnand_recover_page_locked(struct qemu_3dnand *q3n,
					   u32 data_block, u32 page, u8 *buf)
{
	u32 group = data_block / Q3N_RAID_LANES;
	u32 missing_lane = data_block % Q3N_RAID_LANES;
	u32 base = group * Q3N_RAID_LANES;
	struct qemu_3dnand_parity_entry *entry;
	u32 lane;
	u32 i;
	int ret;

	if (group >= q3n->raid_group_count)
		return -EIO;

	entry = &q3n->parity_index[qemu_3dnand_parity_index(q3n, group, page)];
	if (!qemu_3dnand_parity_generation_valid(q3n, entry, group)) {
		qemu_3dnand_mark_parity_stale(q3n, entry);
		return -EIO;
	}

	ret = qemu_3dnand_read_phys_page_locked(q3n, entry->physical_block,
						entry->page, buf);
	if (ret)
		return ret;

	for (lane = 0; lane < Q3N_RAID_LANES; lane++) {
		if (lane == missing_lane)
			continue;
		if (!q3n->data_page_valid[qemu_3dnand_data_page_index(q3n,
								       base + lane,
								       page)])
			return -EIO;
		ret = qemu_3dnand_read_phys_page_locked(q3n, base + lane, page,
							q3n->page_buf);
		if (ret)
			return ret;
		for (i = 0; i < q3n->page_size; i++)
			buf[i] ^= q3n->page_buf[i];
	}

	q3n->raid_recovered++;
	return 0;
}

static int qemu_3dnand_read_data_page_locked(struct qemu_3dnand *q3n,
					     u32 data_block, u32 page, u8 *buf)
{
	int ret;

	ret = qemu_3dnand_read_phys_page_locked(q3n, data_block, page, buf);
	if (!ret)
		return 0;

	ret = qemu_3dnand_recover_page_locked(q3n, data_block, page, buf);
	if (ret)
		q3n->raid_failed++;
	return ret;
}

static int qemu_3dnand_mtd_read(struct mtd_info *mtd, loff_t from, size_t len,
				size_t *retlen, u_char *buf)
{
	struct qemu_3dnand *q3n = mtd->priv;
	size_t done = 0;
	int ret = 0;

	if (from < 0 || from + len > mtd->size)
		return -EINVAL;

	mutex_lock(&q3n->lock);
	while (done < len) {
		u32 block, page, column;
		size_t chunk;

		qemu_3dnand_decode_logical(q3n, from + done, &block, &page,
					   &column);
		chunk = min_t(size_t, len - done, q3n->page_size - column);
		ret = qemu_3dnand_read_data_page_locked(q3n, block, page,
							q3n->page_buf);
		if (ret)
			break;
		memcpy(buf + done, q3n->page_buf + column, chunk);
		done += chunk;
	}
	mutex_unlock(&q3n->lock);

	*retlen = done;
	return ret;
}

static int qemu_3dnand_mtd_write(struct mtd_info *mtd, loff_t to, size_t len,
				 size_t *retlen, const u_char *buf)
{
	struct qemu_3dnand *q3n = mtd->priv;
	size_t done = 0;
	int ret = 0;

	if (to < 0 || to + len > mtd->size)
		return -EINVAL;
	if (!IS_ALIGNED(to, q3n->page_size) || !IS_ALIGNED(len, q3n->page_size))
		return -EINVAL;

	mutex_lock(&q3n->lock);
	while (done < len) {
		u32 block, page, column;
		u32 group;

		qemu_3dnand_decode_logical(q3n, to + done, &block, &page,
					   &column);
		ret = qemu_3dnand_program_phys_page_locked(q3n, block, page,
							   buf + done);
		if (ret)
			break;

		q3n->data_page_valid[qemu_3dnand_data_page_index(q3n, block,
								  page)] = 1;
		q3n->data_meta[block].erased = false;
		group = block / Q3N_RAID_LANES;
		ret = qemu_3dnand_append_parity_locked(q3n, group, page);
		if (ret)
			break;
		done += q3n->page_size;
	}
	mutex_unlock(&q3n->lock);

	*retlen = done;
	return ret;
}

static void qemu_3dnand_invalidate_group_parity(struct qemu_3dnand *q3n,
						u32 group)
{
	u32 page;

	if (group >= q3n->raid_group_count)
		return;

	for (page = 0; page < q3n->pages_per_block; page++) {
		struct qemu_3dnand_parity_entry *entry;

		entry = &q3n->parity_index[qemu_3dnand_parity_index(q3n, group,
								     page)];
		if (!qemu_3dnand_parity_generation_valid(q3n, entry, group))
			qemu_3dnand_mark_parity_stale(q3n, entry);
	}
}

static int qemu_3dnand_mtd_erase(struct mtd_info *mtd,
				 struct erase_info *instr)
{
	struct qemu_3dnand *q3n = mtd->priv;
	u64 done = 0;
	int ret = 0;

	if (instr->addr + instr->len > mtd->size)
		return -EINVAL;
	if (!IS_ALIGNED(instr->addr, mtd->erasesize) ||
	    !IS_ALIGNED(instr->len, mtd->erasesize))
		return -EINVAL;

	mutex_lock(&q3n->lock);
	while (done < instr->len) {
		u32 block, page, column;

		qemu_3dnand_decode_logical(q3n, instr->addr + done, &block,
					   &page, &column);
		ret = qemu_3dnand_erase_phys_block_locked(q3n, block);
		if (ret) {
			instr->fail_addr = instr->addr + done;
			break;
		}
		memset(&q3n->data_page_valid[block * q3n->pages_per_block], 0,
		       q3n->pages_per_block);
		q3n->data_meta[block].generation++;
		if (!q3n->data_meta[block].generation)
			q3n->data_meta[block].generation = 1;
		q3n->data_meta[block].erased = true;
		q3n->generation_updates++;
		qemu_3dnand_invalidate_group_parity(q3n,
						    block / Q3N_RAID_LANES);
		done += mtd->erasesize;
	}
	mutex_unlock(&q3n->lock);

	return ret;
}

static int qemu_3dnand_register_mtd(struct qemu_3dnand *q3n)
{
	struct mtd_info *mtd = &q3n->mtd;

	mtd->name = "qemu-3dnand";
	mtd->type = MTD_NANDFLASH;
	mtd->flags = MTD_CAP_NANDFLASH;
	mtd->size = (u64)q3n->data_block_count * q3n->pages_per_block *
		    q3n->page_size;
	mtd->erasesize = q3n->pages_per_block * q3n->page_size;
	mtd->writesize = q3n->page_size;
	mtd->writebufsize = q3n->page_size;
	mtd->oobsize = q3n->oob_size;
	mtd->owner = THIS_MODULE;
	mtd->priv = q3n;
	mtd->_read = qemu_3dnand_mtd_read;
	mtd->_write = qemu_3dnand_mtd_write;
	mtd->_erase = qemu_3dnand_mtd_erase;
	mtd->dev.parent = &q3n->pdev->dev;

	return mtd_device_register(mtd, NULL, 0);
}

static int qemu_3dnand_inject_data_loss(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;
	int ret;

	mutex_lock(&q3n->lock);
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_LO, lower_32_bits(value));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_HI, upper_32_bits(value));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_CTRL,
			   Q3N_FAULT_INJECT_DATA_LOSS);
	ret = qemu_3dnand_wait_ready(q3n);
	mutex_unlock(&q3n->lock);

	return ret;
}

static int qemu_3dnand_raid_recovered_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->raid_recovered;
	return 0;
}

static int qemu_3dnand_raid_failed_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->raid_failed;
	return 0;
}

static int qemu_3dnand_parity_stale_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->parity_stale;
	return 0;
}

static int qemu_3dnand_faults_injected_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = qemu_3dnand_readl(q3n, Q3N_REG_STAT_FAULTS_INJECTED);
	return 0;
}

static int qemu_3dnand_parity_written_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->parity_written;
	return 0;
}

static int qemu_3dnand_generation_updates_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->generation_updates;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_inject_data_loss_fops, NULL,
			 qemu_3dnand_inject_data_loss, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_raid_recovered_fops,
			 qemu_3dnand_raid_recovered_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_raid_failed_fops,
			 qemu_3dnand_raid_failed_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_stale_fops,
			 qemu_3dnand_parity_stale_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_faults_injected_fops,
			 qemu_3dnand_faults_injected_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_written_fops,
			 qemu_3dnand_parity_written_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_generation_updates_fops,
			 qemu_3dnand_generation_updates_get, NULL, "%llu\n");

static void qemu_3dnand_debugfs_init(struct qemu_3dnand *q3n)
{
	q3n->debugfs_dir = debugfs_create_dir("qemu_3dnand", NULL);
	debugfs_create_file("inject_data_loss", 0200, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_inject_data_loss_fops);
	debugfs_create_file("raid_recovered", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_raid_recovered_fops);
	debugfs_create_file("raid_failed", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_raid_failed_fops);
	debugfs_create_file("parity_stale", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_stale_fops);
	debugfs_create_file("parity_written", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_written_fops);
	debugfs_create_file("generation_updates", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_generation_updates_fops);
	debugfs_create_file("faults_injected", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_faults_injected_fops);
}

static int qemu_3dnand_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct qemu_3dnand *q3n;
	struct resource bar;
	u32 ident;
	u32 cap;
	u32 geom0;
	u32 geom1;
	u32 pool0;
	u32 pool1;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM))
		return dev_err_probe(dev, -ENODEV, "BAR0 is not MMIO\n");

	q3n = devm_kzalloc(dev, sizeof(*q3n), GFP_KERNEL);
	if (!q3n)
		return -ENOMEM;

	memset(&bar, 0, sizeof(bar));
	bar.name = "qemu_3dnand_mmio";
	bar.start = pci_resource_start(pdev, 0);
	bar.end = pci_resource_end(pdev, 0);
	bar.flags = IORESOURCE_MEM;

	q3n->pdev = pdev;
	q3n->regs_size = resource_size(&bar);
	q3n->regs = devm_ioremap_resource(dev, &bar);
	if (IS_ERR(q3n->regs))
		return PTR_ERR(q3n->regs);

	mutex_init(&q3n->lock);
	pci_set_drvdata(pdev, q3n);

	ident = qemu_3dnand_readl(q3n, Q3N_REG_ID);
	if (ident != Q3N_ID_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected q3n id 0x%08x\n", ident);

	cap = qemu_3dnand_readl(q3n, Q3N_REG_CAP);
	geom0 = qemu_3dnand_readl(q3n, Q3N_REG_GEOM0);
	geom1 = qemu_3dnand_readl(q3n, Q3N_REG_GEOM1);
	pool0 = qemu_3dnand_readl(q3n, Q3N_REG_POOL0);
	pool1 = qemu_3dnand_readl(q3n, Q3N_REG_POOL1);

	q3n->page_size = geom0 & 0xffff;
	q3n->oob_size = geom0 >> 16;
	q3n->pages_per_block = geom1 & 0xffff;
	q3n->blocks_per_plane = geom1 >> 16;
	q3n->data_blocks_per_plane = pool0 & 0xffff;
	q3n->parity_blocks_per_plane = pool0 >> 16;
	q3n->metadata_blocks_per_plane = pool1 & 0xffff;
	q3n->reserve_blocks_per_plane = pool1 >> 16;
	q3n->data_block_count = q3n->data_blocks_per_plane * Q3N_RAID_LANES;
	q3n->parity_block_count = q3n->parity_blocks_per_plane * Q3N_RAID_LANES;
	q3n->raid_group_count = q3n->data_block_count / Q3N_RAID_LANES;

	q3n->page_buf = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
	q3n->raid_buf = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
	q3n->data_page_valid = devm_kcalloc(dev,
					    q3n->data_block_count *
					    q3n->pages_per_block,
					    sizeof(*q3n->data_page_valid),
					    GFP_KERNEL);
	q3n->data_meta = devm_kcalloc(dev, q3n->data_block_count,
				      sizeof(*q3n->data_meta), GFP_KERNEL);
	q3n->parity_index = devm_kcalloc(dev,
					 q3n->raid_group_count *
					 q3n->pages_per_block,
					 sizeof(*q3n->parity_index),
					 GFP_KERNEL);
	if (!q3n->page_buf || !q3n->raid_buf || !q3n->data_page_valid ||
	    !q3n->data_meta || !q3n->parity_index)
		return -ENOMEM;

	for (ret = 0; ret < q3n->data_block_count; ret++) {
		q3n->data_meta[ret].generation = 1;
		q3n->data_meta[ret].erased = true;
	}

	ret = qemu_3dnand_register_mtd(q3n);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register MTD\n");
	qemu_3dnand_debugfs_init(q3n);

	dev_info(dev,
		 "q3n NAND: page=%u oob=%u pages/block=%u blocks/plane=%u cap=0x%x\n",
		 q3n->page_size, q3n->oob_size, q3n->pages_per_block,
		 q3n->blocks_per_plane, cap);
	dev_info(dev,
		 "q3n driver RAID: data=%u parity=%u metadata=%u reserve=%u bar=%pa size=%pa mtd=%s\n",
		 q3n->data_blocks_per_plane, q3n->parity_blocks_per_plane,
		 q3n->metadata_blocks_per_plane, q3n->reserve_blocks_per_plane,
		 &bar.start, &q3n->regs_size, q3n->mtd.name);

	return 0;
}

static void qemu_3dnand_remove(struct pci_dev *pdev)
{
	struct qemu_3dnand *q3n = pci_get_drvdata(pdev);

	if (q3n) {
		debugfs_remove_recursive(q3n->debugfs_dir);
		mtd_device_unregister(&q3n->mtd);
	}
	pci_set_drvdata(pdev, NULL);
}

static const struct pci_device_id qemu_3dnand_id_table[] = {
	{ PCI_DEVICE(Q3N_PCI_VENDOR_ID, Q3N_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, qemu_3dnand_id_table);

static struct pci_driver qemu_3dnand_driver = {
	.name = "qemu_3dnand",
	.id_table = qemu_3dnand_id_table,
	.probe = qemu_3dnand_probe,
	.remove = qemu_3dnand_remove,
};
module_pci_driver(qemu_3dnand_driver);

MODULE_DESCRIPTION("QEMU 3D NAND PCI MTD driver with driver-owned page RAID");
MODULE_AUTHOR("OpenAI Codex");
MODULE_LICENSE("GPL");
