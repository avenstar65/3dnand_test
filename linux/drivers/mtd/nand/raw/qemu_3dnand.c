// SPDX-License-Identifier: GPL-2.0
/*
 * Linux MTD driver for the QEMU 3D NAND controller model.
 */

#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mtd/mtd.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "qemu_3dnand.h"

struct qemu_3dnand {
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_size;
	struct mutex lock;
	struct mtd_info mtd;
	u8 *page_buf;
	u32 page_size;
	u32 oob_size;
	u32 pages_per_block;
	u32 blocks_per_plane;
	u32 data_blocks_per_plane;
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

static void qemu_3dnand_set_addr(struct qemu_3dnand *q3n, loff_t addr)
{
	qemu_3dnand_writel(q3n, Q3N_REG_ADDR_LO, lower_32_bits(addr));
	qemu_3dnand_writel(q3n, Q3N_REG_ADDR_HI, upper_32_bits(addr));
}

static int qemu_3dnand_read_page_locked(struct qemu_3dnand *q3n, loff_t from,
					u8 *buf)
{
	u32 *words = (u32 *)buf;
	int ret;
	u32 i;

	qemu_3dnand_set_addr(q3n, from);
	qemu_3dnand_writel(q3n, Q3N_REG_LEN, q3n->page_size);
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE);
	ret = qemu_3dnand_wait_ready(q3n);
	if (ret)
		return ret;

	for (i = 0; i < q3n->page_size / sizeof(u32); i++)
		words[i] = qemu_3dnand_readl(q3n, Q3N_REG_DATA);

	return 0;
}

static int qemu_3dnand_program_page_locked(struct qemu_3dnand *q3n, loff_t to,
					   const u8 *buf)
{
	const u32 *words = (const u32 *)buf;
	u32 i;

	qemu_3dnand_set_addr(q3n, to);
	qemu_3dnand_writel(q3n, Q3N_REG_LEN, q3n->page_size);
	for (i = 0; i < q3n->page_size / sizeof(u32); i++)
		qemu_3dnand_writel(q3n, Q3N_REG_DATA, words[i]);

	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_PROGRAM_PAGE);
	return qemu_3dnand_wait_ready(q3n);
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
		loff_t page_addr = round_down(from + done, q3n->page_size);
		size_t page_off = (from + done) - page_addr;
		size_t chunk = min_t(size_t, len - done,
				     q3n->page_size - page_off);

		ret = qemu_3dnand_read_page_locked(q3n, page_addr,
						   q3n->page_buf);
		if (ret)
			break;

		memcpy(buf + done, q3n->page_buf + page_off, chunk);
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
		ret = qemu_3dnand_program_page_locked(q3n, to + done,
						     buf + done);
		if (ret)
			break;
		done += q3n->page_size;
	}
	mutex_unlock(&q3n->lock);

	*retlen = done;
	return ret;
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
		qemu_3dnand_set_addr(q3n, instr->addr + done);
		qemu_3dnand_writel(q3n, Q3N_REG_LEN, mtd->erasesize);
		qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_ERASE_BLOCK);
		ret = qemu_3dnand_wait_ready(q3n);
		if (ret) {
			instr->fail_addr = instr->addr + done;
			break;
		}
		done += mtd->erasesize;
	}
	mutex_unlock(&q3n->lock);

	return ret;
}

static int qemu_3dnand_register_mtd(struct qemu_3dnand *q3n)
{
	struct mtd_info *mtd = &q3n->mtd;
	u64 data_blocks = (u64)q3n->data_blocks_per_plane * 8;

	mtd->name = "qemu-3dnand";
	mtd->type = MTD_NANDFLASH;
	mtd->flags = MTD_CAP_NANDFLASH;
	mtd->size = data_blocks * q3n->pages_per_block * q3n->page_size;
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
	u32 raid_profile;
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
	raid_profile = qemu_3dnand_readl(q3n, Q3N_REG_RAID_PROFILE);

	q3n->page_size = geom0 & 0xffff;
	q3n->oob_size = geom0 >> 16;
	q3n->pages_per_block = geom1 & 0xffff;
	q3n->blocks_per_plane = geom1 >> 16;
	q3n->data_blocks_per_plane = pool0 & 0xffff;
	q3n->page_buf = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
	if (!q3n->page_buf)
		return -ENOMEM;

	ret = qemu_3dnand_register_mtd(q3n);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register MTD\n");

	dev_info(dev,
		 "q3n NAND: page=%u oob=%u pages/block=%u blocks/plane=%u\n",
		 q3n->page_size, q3n->oob_size, q3n->pages_per_block,
		 q3n->blocks_per_plane);
	dev_info(dev,
		 "q3n pools: data=%u parity=%u metadata=%u reserve=%u raid=%u cap=0x%x bar=%pa size=%pa mtd=%s\n",
		 q3n->data_blocks_per_plane, pool0 >> 16,
		 pool1 & 0xffff, pool1 >> 16, raid_profile, cap, &bar.start,
		 &q3n->regs_size, q3n->mtd.name);

	return 0;
}

static void qemu_3dnand_remove(struct pci_dev *pdev)
{
	struct qemu_3dnand *q3n = pci_get_drvdata(pdev);

	if (q3n)
		mtd_device_unregister(&q3n->mtd);
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

MODULE_DESCRIPTION("QEMU 3D NAND PCI MTD driver");
MODULE_AUTHOR("OpenAI Codex");
MODULE_LICENSE("GPL");
