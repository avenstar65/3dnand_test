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

#include "ytmc_nand.h"

struct nand_flash_dev *q3n_flash_ids_for_id(const u8 *id, size_t len);

#endif
