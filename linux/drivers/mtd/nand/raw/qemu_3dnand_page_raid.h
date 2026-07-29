/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_PAGE_RAID_H
#define QEMU_3DNAND_PAGE_RAID_H

#include "qemu_3dnand_page.h"

const struct q3n_page_ops *q3n_page_raid_get_ops(void);

#endif
