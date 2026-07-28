// SPDX-License-Identifier: GPL-2.0
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "qemu_3dnand_controller.h"
#include "qemu_3dnand_flash.h"
#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_init.h"
#include "qemu_3dnand_priv.h"
#include "qemu_3dnand_regs.h"

int q3n_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct nand_flash_dev *ids;
	struct mtd_info *mtd;
	struct q3n *q3n;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = pcim_iomap_regions(pdev, BIT(0), "qemu_3dnand");
	if (ret)
		return dev_err_probe(dev, ret, "cannot map BAR0\n");

	q3n = devm_kzalloc(dev, sizeof(*q3n), GFP_KERNEL);
	if (!q3n)
		return -ENOMEM;

	q3n->pdev = pdev;
	q3n->regs = pcim_iomap_table(pdev)[0];
	if (!q3n->regs)
		return dev_err_probe(dev, -ENODEV, "BAR0 is not MMIO\n");
	q3n->regs_size = pci_resource_len(pdev, 0);
	q3n->geometry = (struct q3n_geometry) {
		.writesize = Q3N_PAGE_SIZE,
		.oobsize = Q3N_LOGICAL_OOB_SIZE,
		.pages_per_block = Q3N_PAGES_PER_BLOCK,
		.blocks = Q3N_DATA_BLOCKS,
	};
	mutex_init(&q3n->lock);

	ret = q3n_hw_reset(q3n);
	if (ret)
		return dev_err_probe(dev, ret, "controller reset failed\n");

	ret = q3n_hw_read_id(q3n, q3n->id, sizeof(q3n->id));
	if (ret)
		return dev_err_probe(dev, ret, "READ ID failed\n");
	ids = q3n_flash_ids_for_id(q3n->id, sizeof(q3n->id));
	if (!ids)
		return dev_err_probe(dev, -ENODEV,
				     "NAND ID is not in the Q3N whitelist\n");

	nand_controller_init(&q3n->controller);
	q3n->controller.ops = &q3n_controller_ops;
	q3n->chip.controller = &q3n->controller;
	nand_set_controller_data(&q3n->chip, q3n);

	mtd = nand_to_mtd(&q3n->chip);
	mtd->name = "qemu-3dnand";
	mtd->dev.parent = dev;
	mtd->owner = THIS_MODULE;
	mtd->bitflip_threshold = Q3N_ECC_STRENGTH;

	pci_set_drvdata(pdev, q3n);
	ret = nand_scan_with_ids(&q3n->chip, 1, ids);
	if (ret)
		goto err_clear_drvdata;
	q3n->scanned = true;

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret)
		goto err_cleanup_nand;

	dev_info(dev,
		 "YTMC Q3N registered: id=%*phN page=%u oob=%u erase=%u blocks=%u\n",
		 (int)sizeof(q3n->id), q3n->id, mtd->writesize, mtd->oobsize,
		 mtd->erasesize, q3n->geometry.blocks);
	return 0;

err_cleanup_nand:
	nand_cleanup(&q3n->chip);
	q3n->scanned = false;
err_clear_drvdata:
	pci_set_drvdata(pdev, NULL);
	return dev_err_probe(dev, ret, "NAND registration failed\n");
}

void q3n_pci_remove(struct pci_dev *pdev)
{
	struct q3n *q3n = pci_get_drvdata(pdev);

	if (!q3n)
		return;

	if (q3n->scanned) {
		mtd_device_unregister(nand_to_mtd(&q3n->chip));
		nand_cleanup(&q3n->chip);
		q3n->scanned = false;
	}
	pci_set_drvdata(pdev, NULL);
}
