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
#include "qemu_3dnand_layout.h"
#include "qemu_3dnand_multiplane_layout.h"
#include "ytmc_nand.h"

struct q3n_page_ops;

enum q3n_storage_mode {
	Q3N_MODE_IDENTITY,
	Q3N_MODE_PAGE_RAID,
	Q3N_MODE_MULTIPLANE,
};

struct q3n_ecc_result {
	u32 status;
	u32 max_bitflips;
	u32 corrected_bits;
	u32 failed_step;
};

struct q3n_legacy_state {
	u8 data[8];
	u8 data_len;
	u8 data_pos;
	u32 erase_page;
	int error;
	bool erase_pending;
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
	struct q3n_geometry physical_geometry;
	struct q3n_geometry geometry;
	struct q3n_page_profile page_profile;
	struct q3n_flash_topology topology;
	struct q3n_multiplane_profile multiplane_profile;
	enum q3n_storage_mode storage_mode;
	const struct q3n_page_ops *page_ops;
	u8 *parity_scratch;
	u8 *multiplane_oob_scratch;
	u64 logical_size;
	u64 raid_recovered_pages;
#ifndef Q3N_HOST_TEST
	struct nand_flash_dev scan_ids[2];
#endif
	struct q3n_legacy_state legacy;
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
