/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_INIT_H
#define QEMU_3DNAND_INIT_H

#include <linux/pci.h>

int q3n_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id);
void q3n_pci_remove(struct pci_dev *pdev);

#endif
