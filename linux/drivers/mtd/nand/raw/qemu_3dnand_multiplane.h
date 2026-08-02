/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QEMU_3DNAND_MULTIPLANE_H
#define QEMU_3DNAND_MULTIPLANE_H

struct q3n_page_ops;

const struct q3n_page_ops *q3n_multiplane_get_ops(void);

#endif
