// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#include <string.h>
#else
#include <linux/errno.h>
#include <linux/string.h>
#endif

#include "qemu_3dnand_hw_multiplane.h"
#include "qemu_3dnand_multiplane.h"
#include "qemu_3dnand_page.h"
#include "qemu_3dnand_regs.h"

static int q3n_mp_logical_map_page(struct q3n *q3n, u32 logical_page,
				   struct q3n_mp_addr *addr)
{
	return q3n_multiplane_map_page(&q3n->multiplane_profile, logical_page,
				      addr);
}

static int q3n_mp_logical_map_block(struct q3n *q3n, u32 logical_block,
				    struct q3n_mp_addr *addr)
{
	return q3n_multiplane_map_block(&q3n->multiplane_profile, logical_block,
				       addr);
}

static void q3n_multiplane_aggregate(const struct q3n_mp_result *transport,
				     struct q3n_page_result *result)
{
	struct q3n_page_result aggregate = { 0 };
	u32 plane;

	for (plane = 0; plane < Q3N_MP_PLANES; plane++) {
		const struct q3n_ecc_result *ecc = &transport->plane[plane].ecc;

		if (ecc->max_bitflips > aggregate.max_bitflips)
			aggregate.max_bitflips = ecc->max_bitflips;
		aggregate.corrected_bits += ecc->corrected_bits;
		if (ecc->status & Q3N_ECC_STATUS_UNCORRECTABLE)
			aggregate.failed_plane_mask |= BIT(plane);
	}

	for (plane = 0; plane < Q3N_MP_PLANES; plane++)
		if (aggregate.failed_plane_mask & BIT(plane))
			aggregate.failed_data_pages++;
	if (result)
		*result = aggregate;
}

static int q3n_multiplane_read_page(struct q3n *q3n, u32 logical_page,
				    void *data, void *oob, bool raw,
				    struct q3n_page_result *result)
{
	struct q3n_mp_addr addr;
	struct q3n_mp_result transport;
	struct q3n_page_result aggregate;
	int ret;

	ret = q3n_mp_logical_map_page(q3n, logical_page, &addr);
	if (ret)
		return ret;
	ret = q3n_hw_mp_read_page(q3n, &addr, data, raw, &transport);
	if (ret)
		return ret;
	if (!raw)
		q3n_multiplane_aggregate(&transport, &aggregate);
	if (oob) {
		ret = q3n_hw_mp_read_oob(q3n, &addr, oob, &transport);
		if (ret)
			return ret;
		if (!addr.page_in_block)
			((u8 *)oob)[0] = q3n_multiplane_bbm_fold(oob);
	}
	if (raw) {
		if (result)
			memset(result, 0, sizeof(*result));
		return 0;
	}
	if (result)
		*result = aggregate;
	return 0;
}

static int q3n_multiplane_write_page(struct q3n *q3n, u32 logical_page,
				     const void *data, const void *oob)
{
	struct q3n_mp_addr addr;
	struct q3n_mp_result transport;
	const void *program_oob = oob;
	int ret;

	ret = q3n_mp_logical_map_page(q3n, logical_page, &addr);
	if (ret)
		return ret;
	ret = q3n_hw_mp_program_page(q3n, &addr, data, &transport);
	if (ret || !program_oob)
		return ret;
	if (!addr.page_in_block) {
		if (!q3n->multiplane_oob_scratch)
			return -ENOMEM;
		memcpy(q3n->multiplane_oob_scratch, program_oob,
		       q3n->geometry.oobsize);
		q3n_multiplane_bbm_normalize(q3n->multiplane_oob_scratch);
		program_oob = q3n->multiplane_oob_scratch;
	}
	return q3n_hw_mp_program_oob(q3n, &addr, program_oob, &transport);
}

static int q3n_multiplane_read_oob(struct q3n *q3n, u32 logical_page,
				   void *oob)
{
	struct q3n_mp_addr addr;
	struct q3n_mp_result transport;
	int ret;

	ret = q3n_mp_logical_map_page(q3n, logical_page, &addr);
	if (ret)
		return ret;
	ret = q3n_hw_mp_read_oob(q3n, &addr, oob, &transport);
	if (ret)
		return ret;
	if (!addr.page_in_block)
		((u8 *)oob)[0] = q3n_multiplane_bbm_fold(oob);
	return 0;
}

static int q3n_multiplane_write_oob(struct q3n *q3n, u32 logical_page,
				    const void *oob)
{
	struct q3n_mp_addr addr;
	struct q3n_mp_result transport;
	const void *program_oob = oob;
	int ret;

	ret = q3n_mp_logical_map_page(q3n, logical_page, &addr);
	if (ret)
		return ret;
	if (!addr.page_in_block) {
		if (!q3n->multiplane_oob_scratch)
			return -ENOMEM;
		memcpy(q3n->multiplane_oob_scratch, program_oob,
		       q3n->geometry.oobsize);
		q3n_multiplane_bbm_normalize(q3n->multiplane_oob_scratch);
		program_oob = q3n->multiplane_oob_scratch;
	}
	return q3n_hw_mp_program_oob(q3n, &addr, program_oob, &transport);
}

static int q3n_multiplane_erase_block(struct q3n *q3n, u32 logical_block)
{
	struct q3n_mp_addr addr;
	struct q3n_mp_result transport;
	int ret;

	ret = q3n_mp_logical_map_block(q3n, logical_block, &addr);
	if (ret)
		return ret;
	return q3n_hw_mp_erase_group(q3n, &addr, &transport);
}

static const struct q3n_page_ops q3n_multiplane_page_ops = {
	.read_page = q3n_multiplane_read_page,
	.write_page = q3n_multiplane_write_page,
	.read_oob = q3n_multiplane_read_oob,
	.write_oob = q3n_multiplane_write_oob,
	.erase_block = q3n_multiplane_erase_block,
};

const struct q3n_page_ops *q3n_multiplane_get_ops(void)
{
	return &q3n_multiplane_page_ops;
}
