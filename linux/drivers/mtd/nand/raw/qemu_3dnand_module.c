// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>

#include "qemu_3dnand_init.h"
#include "qemu_3dnand_regs.h"

static const struct pci_device_id q3n_pci_ids[] = {
	{ PCI_DEVICE(Q3N_PCI_VENDOR_ID, Q3N_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, q3n_pci_ids);

static struct pci_driver q3n_pci_driver = {
	.name = "qemu_3dnand",
	.id_table = q3n_pci_ids,
	.probe = q3n_pci_probe,
	.remove = q3n_pci_remove,
};
module_pci_driver(q3n_pci_driver);

MODULE_DESCRIPTION("QEMU 3D NAND raw NAND controller driver");
MODULE_AUTHOR("OpenAI Codex");
MODULE_LICENSE("GPL");
