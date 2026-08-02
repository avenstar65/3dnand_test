// SPDX-License-Identifier: GPL-2.0
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "qemu_3dnand_controller.h"
#include "qemu_3dnand_flash.h"
#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_init.h"
#include "qemu_3dnand_page.h"
#include "qemu_3dnand_priv.h"
#include "qemu_3dnand_regs.h"

static int q3n_set_physical_geometry(struct q3n *q3n,
				     const struct nand_flash_dev *ids)
{
	u64 capacity;
	u64 blocks;

	if (!ids->pagesize || !ids->oobsize || !ids->erasesize ||
	    !ids->chipsize || ids->erasesize % ids->pagesize)
		return -EINVAL;

	capacity = (u64)ids->chipsize * 1024 * 1024;
	if (capacity % ids->erasesize)
		return -EINVAL;

	blocks = capacity / ids->erasesize;
	if (blocks > UINT_MAX)
		return -EOVERFLOW;

	q3n->physical_geometry = (struct q3n_geometry) {
		.writesize = ids->pagesize,
		.oobsize = ids->oobsize,
		.pages_per_block = ids->erasesize / ids->pagesize,
		.blocks = blocks,
	};
	return 0;
}

static int q3n_set_topology(struct q3n *q3n,
				    const struct q3n_flash_topology *topology)
{
	u64 data_blocks;

	if (!topology || !topology->dies || !topology->planes_per_die ||
	    !topology->blocks_per_plane || !topology->data_blocks_per_plane ||
	    !topology->pages_per_block ||
	    topology->data_blocks_per_plane > topology->blocks_per_plane ||
	    topology->pages_per_block != q3n->physical_geometry.pages_per_block)
		return -EINVAL;

	data_blocks = (u64)topology->dies * topology->planes_per_die *
		topology->data_blocks_per_plane;
	if (data_blocks != q3n->physical_geometry.blocks)
		return -EINVAL;

	q3n->topology = *topology;
	return 0;
}

static enum q3n_storage_mode q3n_build_storage_mode(void)
{
#if IS_ENABLED(CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE)
	return Q3N_MODE_MULTIPLANE;
#elif IS_ENABLED(CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID)
	return Q3N_MODE_PAGE_RAID;
#else
	return Q3N_MODE_IDENTITY;
#endif
}

int q3n_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	const struct q3n_flash_info *info;
	const struct nand_flash_dev *physical_ids;
	struct mtd_info *mtd;
	struct q3n *q3n;
	enum q3n_storage_mode storage_mode;
	u32 capabilities;
	u32 data_pages;
	u32 used_pages;
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
	mutex_init(&q3n->lock);

	ret = q3n_hw_reset(q3n);
	if (ret)
		return dev_err_probe(dev, ret, "controller reset failed\n");

	ret = q3n_hw_read_id(q3n, q3n->id, sizeof(q3n->id));
	if (ret)
		return dev_err_probe(dev, ret, "READ ID failed\n");
	info = q3n_flash_info_for_id(q3n->id, sizeof(q3n->id));
	if (!info)
		return dev_err_probe(dev, -ENODEV,
				     "NAND ID is not in the Q3N whitelist\n");
	physical_ids = &info->nand;

	ret = q3n_set_physical_geometry(q3n, physical_ids);
	if (ret)
		return dev_err_probe(dev, ret,
				     "invalid physical NAND geometry\n");

	ret = q3n_set_topology(q3n, &info->topology);
	if (ret)
		return dev_err_probe(dev, ret, "invalid YTMC NAND topology\n");

	storage_mode = q3n_build_storage_mode();
	capabilities = q3n_hw_read_capabilities(q3n);
	if (storage_mode == Q3N_MODE_MULTIPLANE &&
	    !(capabilities & Q3N_CAP_MULTIPLANE))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "multi-plane capability is unavailable\n");

#if IS_ENABLED(CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID)
	data_pages = CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES;
#else
	data_pages = 1;
#endif
	ret = q3n_page_layer_init(q3n, storage_mode, data_pages,
			  &q3n->topology);
	if (ret)
		return dev_err_probe(dev, ret,
				     "cannot initialize logical page layer\n");

	ret = q3n_flash_build_scan_ids(q3n->scan_ids, info, &q3n->geometry);
	if (ret)
		goto err_cleanup_page_layer;

	nand_controller_init(&q3n->controller);
	q3n->controller.ops = &q3n_controller_ops;
	q3n->chip.controller = &q3n->controller;
	nand_set_controller_data(&q3n->chip, q3n);
	q3n_controller_legacy_init(&q3n->chip);

	mtd = nand_to_mtd(&q3n->chip);
	mtd->name = "qemu-3dnand";
	mtd->dev.parent = dev;
	mtd->owner = THIS_MODULE;
	mtd->bitflip_threshold = Q3N_ECC_STRENGTH;

	pci_set_drvdata(pdev, q3n);
	ret = nand_scan_with_ids(&q3n->chip, 1, q3n->scan_ids);
	if (ret)
		goto err_clear_drvdata;
	q3n->scanned = true;

	if (storage_mode == Q3N_MODE_MULTIPLANE) {
		if (mtd->oobavail != 4092) {
			ret = -EINVAL;
			goto err_cleanup_nand;
		}
		dev_info(dev, "mode=multiplane dies=2 planes/group=4\n");
		dev_info(dev,
			 "physical-page=16384 logical-page=65536 logical-oob=4096\n");
		dev_info(dev,
			 "pages/block=1600 logical-erasesize=104857600 logical-blocks=416\n");
		dev_info(dev,
			 "logical-size=43620761600 image-mode=multiplane\n");
	} else {
		used_pages = q3n->page_profile.stripes_per_block *
			q3n->page_profile.stripe_pages;
		dev_info(dev,
			 "Page RAID %s: %u data + %u parity, logical page=%u physical page=%u oob=%u erase=%u capacity=%llu, stripes/block=%u used pages/block=%u tail pages/block=%u\n",
			 storage_mode == Q3N_MODE_PAGE_RAID ? "enabled" : "disabled",
			 q3n->page_profile.data_pages,
			 q3n->page_profile.parity_pages,
			 q3n->geometry.writesize,
			 q3n->physical_geometry.writesize, q3n->geometry.oobsize,
			 mtd->erasesize,
			 (unsigned long long)q3n->logical_size,
			 q3n->page_profile.stripes_per_block, used_pages,
			 q3n->page_profile.tail_pages);
	}

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
err_cleanup_page_layer:
	q3n_page_layer_cleanup(q3n);
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
	q3n_page_layer_cleanup(q3n);
	pci_set_drvdata(pdev, NULL);
}
