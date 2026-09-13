// SPDX-License-Identifier: GPL-2.0
/* PCI lifecycle for the QEMU 3D NAND Linux driver. */

#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "qemu_3dnand_internal.h"

static unsigned int raid_level = Q3N_RAID5;
module_param(raid_level, uint, 0444);
MODULE_PARM_DESC(raid_level, "Page RAID level: 1 or 5");

static void qemu_3dnand_free_metadata(struct qemu_3dnand *q3n)
{
	kvfree(q3n->profile_state);
	q3n->profile_state = NULL;
	kvfree(q3n->data_page_valid);
	q3n->data_page_valid = NULL;
	kvfree(q3n->parity_index);
	q3n->parity_index = NULL;
}

static int q3n_pci_prepare(struct pci_dev *pdev, struct qemu_3dnand **out)
{
	struct device *dev = &pdev->dev;
	struct qemu_3dnand *q3n;
	struct resource bar = {
		.name = "qemu_3dnand_mmio",
		.start = pci_resource_start(pdev, 0),
		.end = pci_resource_end(pdev, 0),
		.flags = IORESOURCE_MEM,
	};
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM))
		return dev_err_probe(dev, -ENODEV, "BAR0 is not MMIO\n");
	q3n = devm_kzalloc(dev, sizeof(*q3n), GFP_KERNEL);
	if (!q3n)
		return -ENOMEM;
	q3n->pdev = pdev;
	q3n->regs_size = resource_size(&bar);
	q3n->regs = devm_ioremap_resource(dev, &bar);
	if (IS_ERR(q3n->regs))
		return PTR_ERR(q3n->regs);
	mutex_init(&q3n->mtd_lock);
	q3n_sched_init(&q3n->sched);
	init_waitqueue_head(&q3n->parity_cancel_waitq);
	init_waitqueue_head(&q3n->parity_pause_waitq);
	atomic_set(&q3n->parity_paused, 0);
	atomic_set(&q3n->parity_continuation_paused, 0);
	atomic_set(&q3n->fail_next_parity_queue, 0);
	atomic64_set(&q3n->protected_stripes, 0);
	atomic64_set(&q3n->unprotected_stripes, 0);
	atomic64_set(&q3n->failed_stripes, 0);
	q3n->parity_continuation_pause_class = Q3N_REQ_PARITY_READ;
	q3n->raid_level = raid_level;
	*out = q3n;
	return 0;
}

static int q3n_validate_controller(struct qemu_3dnand *q3n)
{
	struct device *dev = &q3n->pdev->dev;
	u32 ident = q3n_hw_readl(q3n, Q3N_REG_ID);

	if (ident != Q3N_ID_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected q3n id 0x%08x\n", ident);
	q3n->cap = q3n_hw_readl(q3n, Q3N_REG_CAP);
#if Q3N_ENABLE_MULTIPLANE_RAID
	if (raid_level != Q3N_RAID1 && raid_level != Q3N_RAID5)
		return dev_err_probe(dev, -EINVAL,
				     "raid_level must be 1 or 5\n");
	if (!(q3n->cap & Q3N_CAP_MULTIPLANE))
		return dev_err_probe(dev, -ENODEV,
				     "controller lacks multi-plane support\n");
#endif
	return 0;
}

static int q3n_read_geometry(struct qemu_3dnand *q3n)
{
	struct q3n_device_geometry observed;
	u32 geom0 = q3n_hw_readl(q3n, Q3N_REG_GEOM0);
	u32 geom1 = q3n_hw_readl(q3n, Q3N_REG_GEOM1);
	u32 ecc_geom0 = q3n_hw_readl(q3n, Q3N_REG_ECC_GEOM0);
	u32 ecc_geom1 = q3n_hw_readl(q3n, Q3N_REG_ECC_GEOM1);
	u32 pool0 = q3n_hw_readl(q3n, Q3N_REG_POOL0);
	u32 pool1 = q3n_hw_readl(q3n, Q3N_REG_POOL1);

	observed = (struct q3n_device_geometry) {
		.page_size = geom0 & 0xffff,
		.oob_size = geom0 >> 16,
		.pages_per_block = geom1 & 0xffff,
		.blocks_per_plane = geom1 >> 16,
		.ecc_step_size = ecc_geom0 & 0xffff,
		.ecc_strength = ecc_geom0 >> 16,
		.ldpc_bytes_per_step = ecc_geom1 & 0xffff,
		.ldpc_steps = ecc_geom1 >> 16,
	};
	if (q3n_device_validate_geometry(q3n, &observed))
		return -EINVAL;
	q3n->data_blocks_per_plane = pool0 & 0xffff;
	q3n->parity_blocks_per_plane = pool0 >> 16;
	q3n->metadata_blocks_per_plane = pool1 & 0xffff;
	q3n->reserve_blocks_per_plane = pool1 >> 16;
	return 0;
}

static int q3n_discover_device(struct qemu_3dnand *q3n)
{
	struct device *dev = &q3n->pdev->dev;
	int ret;

	mutex_lock(&q3n->mtd_lock);
	ret = q3n_hw_read_id_locked(q3n, q3n->nand_id, Q3N_NAND_ID_LEN);
	mutex_unlock(&q3n->mtd_lock);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read NAND ID\n");
	q3n->nand_id_len = Q3N_NAND_ID_LEN;
	q3n->device = q3n_device_match(q3n->nand_id, q3n->nand_id_len);
	if (!q3n->device)
		return dev_err_probe(dev, -ENODEV, "unsupported NAND ID\n");
	ret = q3n_device_apply_geometry(q3n, q3n->device);
	return ret ? dev_err_probe(dev, ret, "invalid NAND descriptor\n") : 0;
}

static int q3n_validate_device_geometry(struct qemu_3dnand *q3n)
{
	int ret = q3n_device_validate(q3n, q3n->device);

	if (!ret)
		return 0;
	return dev_err_probe(&q3n->pdev->dev, ret, "NAND geometry mismatch\n");
}

static void q3n_configure_geometry(struct qemu_3dnand *q3n)
{
	q3n->mp.regs = q3n->regs;
	q3n->mp.page_size = q3n->page_size;
	q3n->mp.oob_size = q3n->oob_size;
	q3n->mp.pages_per_block = q3n->pages_per_block;
	q3n->mp.dies = q3n->profile_geometry.dies;
	q3n->mp.planes_per_die = q3n->profile_geometry.planes_per_die;
	q3n->profile_geometry = (struct q3n_geometry) {
		.page_size = q3n->page_size,
		.pages_per_block = q3n->pages_per_block,
		.blocks_per_plane = q3n->blocks_per_plane,
		.data_blocks_per_plane = q3n->data_blocks_per_plane,
		.dies = q3n->profile_geometry.dies,
		.planes_per_die = q3n->profile_geometry.planes_per_die,
		.raid_level = q3n->raid_level,
	};
	q3n->profile_leb_count = q3n->data_blocks_per_plane *
		(q3n->raid_level == Q3N_RAID1 ? 4 : 2);
	q3n->last_parity_plane = Q3N_RAID_NO_PARITY;
	q3n->data_block_count = q3n->data_blocks_per_plane * Q3N_RAID_LANES;
	q3n->parity_block_count = q3n->parity_blocks_per_plane * Q3N_RAID_LANES;
	q3n->raid_group_count = q3n->data_block_count / Q3N_RAID_LANES;
	q3n->profile_geometry.data_pages_per_stripe = Q3N_DATA_PAGES;
	q3n->profile_geometry.data_block_count = q3n->data_block_count;
	q3n->profile_geometry.parity_block_count = q3n->parity_block_count;
}

static int q3n_alloc_buffers(struct qemu_3dnand *q3n)
{
	struct device *dev = &q3n->pdev->dev;
	u8 plane;

	q3n->page_buf = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
	q3n->raid_buf = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
	if (!q3n->page_buf || !q3n->raid_buf)
		return -ENOMEM;
	for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++) {
		q3n->mp_buf[plane] = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
		q3n->mp_oob[plane] = devm_kmalloc(dev, q3n->oob_size,
						 GFP_KERNEL);
		if (!q3n->mp_buf[plane] || !q3n->mp_oob[plane])
			return -ENOMEM;
	}
	return 0;
}

static int q3n_alloc_metadata(struct qemu_3dnand *q3n)
{
	struct device *dev = &q3n->pdev->dev;
	u32 block;

	q3n->profile_state = kvcalloc((u64)q3n->profile_leb_count *
		q3n->pages_per_block, sizeof(*q3n->profile_state), GFP_KERNEL);
	q3n->data_page_valid = kvcalloc(q3n->data_block_count,
		q3n->pages_per_block, GFP_KERNEL);
	q3n->data_meta = devm_kcalloc(dev, q3n->data_block_count,
		sizeof(*q3n->data_meta), GFP_KERNEL);
	q3n->parity_index = kvcalloc(q3n->data_block_count *
		(q3n->pages_per_block / Q3N_STRIPE_PAGES),
		sizeof(*q3n->parity_index), GFP_KERNEL);
	if (!q3n->profile_state || !q3n->data_page_valid || !q3n->data_meta ||
	    !q3n->parity_index)
		return -ENOMEM;
	for (block = 0; block < q3n->data_block_count; block++) {
		q3n->data_meta[block].generation = 1;
		q3n_block_barrier_init(&q3n->data_meta[block].parity_barrier);
	}
	return 0;
}

static int q3n_alloc_runtime(struct qemu_3dnand *q3n)
{
	int ret;

	q3n->parity_wq = alloc_workqueue("q3n-parity", WQ_UNBOUND,
					 Q3N_MAX_PENDING_PARITY);
	if (!q3n->parity_wq)
		return -ENOMEM;
	ret = q3n_alloc_buffers(q3n);
	return ret ?: q3n_alloc_metadata(q3n);
}

static void q3n_log_geometry(struct qemu_3dnand *q3n)
{
	struct device *dev = &q3n->pdev->dev;
	resource_size_t bar_start = pci_resource_start(q3n->pdev, 0);

	dev_info(dev,
		 "q3n NAND %s: page=%u oob=%u physical-oob=%u pages/block=%u blocks/plane=%u cap=0x%x\n",
		 q3n_device_name(q3n->device),
		 q3n->page_size, q3n->oob_size, q3n->physical_oob_size,
		 q3n->pages_per_block,
		 q3n->blocks_per_plane, q3n->cap);
	dev_info(dev,
		 "q3n driver %s: data=%u parity=%u metadata=%u reserve=%u bar=%pa size=%pa mtd=%s\n",
		 Q3N_ENABLE_MULTIPLANE_RAID ?
			(q3n->raid_level == Q3N_RAID1 ? "multi-plane RAID1" :
			 "multi-plane RAID5") : "serial RAID",
		 q3n->data_blocks_per_plane, q3n->parity_blocks_per_plane,
		 q3n->metadata_blocks_per_plane, q3n->reserve_blocks_per_plane,
		 &bar_start, &q3n->regs_size, q3n->mtd->name);
}

static int q3n_probe_failed(struct qemu_3dnand *q3n, int ret)
{
	if (q3n->parity_wq)
		destroy_workqueue(q3n->parity_wq);
	qemu_3dnand_free_metadata(q3n);
	return dev_err_probe(&q3n->pdev->dev, ret,
			     "failed to initialize NAND\n");
}

static int qemu_3dnand_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct qemu_3dnand *q3n;
	int ret;

	ret = q3n_pci_prepare(pdev, &q3n);
	if (ret)
		return ret;
	ret = q3n_validate_controller(q3n);
	if (!ret)
		ret = q3n_discover_device(q3n);
	if (!ret)
		ret = q3n_read_geometry(q3n);
	if (!ret)
		ret = q3n_validate_device_geometry(q3n);
	if (ret)
		return ret;
	q3n_configure_geometry(q3n);
	ret = q3n_alloc_runtime(q3n);
	if (!ret)
		ret = q3n_nand_register(q3n);
	if (ret)
		return q3n_probe_failed(q3n, ret);
	pci_set_drvdata(pdev, q3n);
	q3n_debugfs_init(q3n);
	q3n_log_geometry(q3n);

	return 0;
}

static void qemu_3dnand_remove(struct pci_dev *pdev)
{
	struct qemu_3dnand *q3n = pci_get_drvdata(pdev);

	if (q3n) {
		q3n_debugfs_remove(q3n);
		q3n_debugfs_unpause(q3n);
		q3n_nand_unregister(q3n);
		flush_workqueue(q3n->parity_wq);
		q3n_nand_cleanup(q3n);
		destroy_workqueue(q3n->parity_wq);
		qemu_3dnand_free_metadata(q3n);
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
