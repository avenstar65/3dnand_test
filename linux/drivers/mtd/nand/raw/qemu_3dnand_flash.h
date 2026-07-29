/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_FLASH_H
#define QEMU_3DNAND_FLASH_H

#ifdef Q3N_HOST_TEST
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
#else
#include <linux/types.h>
#endif

#include "qemu_3dnand_layout.h"
#include "ytmc_nand.h"

struct nand_flash_dev *q3n_flash_ids_for_id(const u8 *id, size_t len);
int q3n_flash_build_scan_ids(struct nand_flash_dev scan_ids[2],
			     const struct nand_flash_dev *physical_ids,
			     const struct q3n_page_profile *profile);

#endif
