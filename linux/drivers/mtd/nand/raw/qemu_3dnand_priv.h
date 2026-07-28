/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_PRIV_H
#define QEMU_3DNAND_PRIV_H

#ifdef Q3N_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include <linux/mtd/rawnand.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/types.h>
#endif

#include "qemu_3dnand_addr.h"

struct q3n_ecc_result {
	u32 status;
	u32 max_bitflips;
	u32 corrected_bits;
	u32 failed_step;
};

#ifdef Q3N_HOST_TEST
struct q3n_mmio {
	u32 (*read)(void *context, u32 reg);
	void (*write)(void *context, u32 reg, u32 value);
	void *context;
};
#endif

struct q3n {
#ifdef Q3N_HOST_TEST
	struct q3n_mmio mmio;
#else
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_size;
	struct mutex lock;
	struct nand_controller controller;
	struct nand_chip chip;
#endif
	struct q3n_geometry geometry;
	u8 id[8];
	u32 retry_mode;
	bool scanned;
};

#ifndef Q3N_HOST_TEST
static inline struct q3n *nand_to_q3n(struct nand_chip *chip)
{
	return nand_get_controller_data(chip);
}
#endif

#endif
