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

#include "qemu_3dnand_addr.h"
#include "ytmc_nand.h"

const struct q3n_flash_info *q3n_flash_info_for_id(const u8 *id, size_t len);
int q3n_flash_build_scan_ids(struct nand_flash_dev scan_ids[2],
			     const struct q3n_flash_info *info,
			     const struct q3n_geometry *logical);

#endif
