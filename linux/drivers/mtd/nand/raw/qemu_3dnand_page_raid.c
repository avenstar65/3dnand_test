// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#include <string.h>
#else
#include <linux/errno.h>
#include <linux/string.h>
#endif

#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_page_raid.h"
#include "qemu_3dnand_regs.h"

static int q3n_raid_map(struct q3n *q3n, u32 logical_page,
			struct q3n_page_map *map)
{
	return q3n_layout_map_page(&q3n->page_profile, logical_page, map);
}

static int q3n_raid_program_data_page(struct q3n *q3n, u32 page,
				      const u8 *data, const u8 *oob)
{
	if (!q3n_page_buffer_erased(data, Q3N_PAGE_SIZE))
		return q3n_hw_program_page(q3n, page, data, oob);
	if (q3n_page_buffer_erased(oob, Q3N_LOGICAL_OOB_SIZE))
		return 0;
	return q3n_hw_program_oob(q3n, page, oob);
}

static int q3n_raid_write_page(struct q3n *q3n, u32 logical_page,
			       const void *data, const void *oob)
{
	const u8 *logical_data = data;
	const u8 *logical_oob = oob;
	struct q3n_page_map map;
	u32 data_page;
	size_t offset;
	int ret;

	ret = q3n_raid_map(q3n, logical_page, &map);
	if (ret)
		return ret;

	memset(q3n->parity_scratch, 0, Q3N_PAGE_SIZE);
	for (data_page = 0; data_page < q3n->page_profile.data_pages;
	     data_page++) {
		const u8 *data_slice =
			logical_data + data_page * Q3N_PAGE_SIZE;
		const u8 *oob_slice = logical_oob ?
			logical_oob +
			data_page * Q3N_LOGICAL_OOB_SIZE : NULL;

		for (offset = 0; offset < Q3N_PAGE_SIZE; offset++)
			q3n->parity_scratch[offset] ^= data_slice[offset];

		ret = q3n_raid_program_data_page(q3n,
				map.first_data_page + data_page,
				data_slice, oob_slice);
		if (ret)
			return ret;
	}

	if (q3n_page_buffer_erased(q3n->parity_scratch, Q3N_PAGE_SIZE))
		return 0;
	return q3n_hw_program_page(q3n, map.parity_page,
				   q3n->parity_scratch, NULL);
}

static int q3n_raid_read_page(struct q3n *q3n, u32 logical_page,
			      void *data, void *oob, bool raw,
			      struct q3n_page_result *result)
{
	u8 *logical_data = data;
	u8 *logical_oob = oob;
	struct q3n_page_result aggregate = { 0 };
	struct q3n_page_map map;
	u32 failed_index = 0;
	u32 data_page;
	int ret;

	ret = q3n_raid_map(q3n, logical_page, &map);
	if (ret)
		return ret;

	for (data_page = 0; data_page < q3n->page_profile.data_pages;
	     data_page++) {
		struct q3n_ecc_result ecc = { 0 };
		u8 *data_slice = logical_data + data_page * Q3N_PAGE_SIZE;
		u8 *oob_slice = logical_oob ?
			logical_oob +
			data_page * Q3N_LOGICAL_OOB_SIZE : NULL;

		ret = q3n_hw_read_page(q3n, map.first_data_page + data_page,
				       data_slice, oob_slice, raw, &ecc);
		if (ret)
			return ret;
		if (raw)
			continue;

		if (ecc.max_bitflips > aggregate.max_bitflips)
			aggregate.max_bitflips = ecc.max_bitflips;
		aggregate.corrected_bits += ecc.corrected_bits;
		if (ecc.status & Q3N_ECC_STATUS_UNCORRECTABLE) {
			failed_index = data_page;
			aggregate.failed_data_pages++;
		}
	}

	if (raw || aggregate.failed_data_pages != 1 || q3n->retry_mode != 3)
		goto out;

	{
		struct q3n_ecc_result parity_ecc = { 0 };
		u8 *missing =
			logical_data + failed_index * Q3N_PAGE_SIZE;
		size_t offset;

		ret = q3n_hw_read_page(q3n, map.parity_page,
				       q3n->parity_scratch, NULL, false,
				       &parity_ecc);
		if (ret)
			return ret;
		if (parity_ecc.status & Q3N_ECC_STATUS_UNCORRECTABLE) {
			aggregate.parity_failed = true;
			goto out;
		}

		memcpy(missing, q3n->parity_scratch, Q3N_PAGE_SIZE);
		for (data_page = 0;
		     data_page < q3n->page_profile.data_pages;
		     data_page++) {
			const u8 *data_slice;

			if (data_page == failed_index)
				continue;
			data_slice = logical_data +
				data_page * Q3N_PAGE_SIZE;
			for (offset = 0; offset < Q3N_PAGE_SIZE; offset++)
				missing[offset] ^= data_slice[offset];
		}

		aggregate.failed_data_pages = 0;
		aggregate.recovered = true;
		if (aggregate.max_bitflips < Q3N_ECC_STRENGTH)
			aggregate.max_bitflips = Q3N_ECC_STRENGTH;
		q3n->raid_recovered_pages++;
	}

out:
	if (result)
		*result = aggregate;
	return 0;
}

static int q3n_raid_read_oob(struct q3n *q3n, u32 logical_page, void *oob)
{
	u8 *logical_oob = oob;
	struct q3n_page_map map;
	u32 data_page;
	int ret;

	ret = q3n_raid_map(q3n, logical_page, &map);
	if (ret)
		return ret;

	for (data_page = 0; data_page < q3n->page_profile.data_pages;
	     data_page++) {
		ret = q3n_hw_read_oob(q3n, map.first_data_page + data_page,
			logical_oob + data_page * Q3N_LOGICAL_OOB_SIZE);
		if (ret)
			return ret;
	}
	return 0;
}

static int q3n_raid_write_oob(struct q3n *q3n, u32 logical_page,
			      const void *oob)
{
	const u8 *logical_oob = oob;
	struct q3n_page_map map;
	u32 data_page;
	int ret;

	ret = q3n_raid_map(q3n, logical_page, &map);
	if (ret)
		return ret;

	for (data_page = 0; data_page < q3n->page_profile.data_pages;
	     data_page++) {
		const u8 *oob_slice = logical_oob +
			data_page * Q3N_LOGICAL_OOB_SIZE;

		if (q3n_page_buffer_erased(oob_slice,
					   Q3N_LOGICAL_OOB_SIZE))
			continue;
		ret = q3n_hw_program_oob(q3n,
			map.first_data_page + data_page,
			oob_slice);
		if (ret)
			return ret;
	}
	return 0;
}

static int q3n_raid_erase_block(struct q3n *q3n, u32 logical_block)
{
	return q3n_hw_erase_block(q3n, logical_block);
}

static const struct q3n_page_ops q3n_raid_page_ops = {
	.read_page = q3n_raid_read_page,
	.write_page = q3n_raid_write_page,
	.read_oob = q3n_raid_read_oob,
	.write_oob = q3n_raid_write_oob,
	.erase_block = q3n_raid_erase_block,
};

const struct q3n_page_ops *q3n_page_raid_get_ops(void)
{
	return &q3n_raid_page_ops;
}
