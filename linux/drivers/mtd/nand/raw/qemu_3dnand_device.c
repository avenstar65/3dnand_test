// SPDX-License-Identifier: GPL-2.0
/* NAND identification and per-device constraints. */

#include <linux/kernel.h>
#include <linux/string.h>

#include "qemu_3dnand_internal.h"

struct q3n_device_desc {
	const char *name;
	u8 id[Q3N_NAND_ID_LEN];
	u8 id_len;
	u32 required_caps;
	u32 page_size;
	u32 logical_oob_size;
	u32 physical_oob_size;
	u32 pages_per_block;
	u32 blocks_per_plane;
	u8 dies;
	u8 planes_per_die;
	u32 ecc_step_size;
	u32 ecc_strength;
	u32 ldpc_bytes_per_step;
	u32 ldpc_steps;
};

static const struct q3n_device_desc q3n_devices[] = {
	{
		.name = "YMTC QEMU 3D NAND",
		.id = { 0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44 },
		.id_len = Q3N_NAND_ID_LEN,
		.required_caps = Q3N_CAP_BASIC_FLASH |
			Q3N_CAP_PERSISTENT_MEDIA | Q3N_CAP_BAD_BLOCK_MARKER,
		.page_size = Q3N_PAGE_SIZE,
		.logical_oob_size = Q3N_LOGICAL_OOB_SIZE,
		.physical_oob_size = Q3N_PHYSICAL_OOB_SIZE,
		.pages_per_block = 1600,
		.blocks_per_plane = 247,
		.dies = Q3N_DIES,
		.planes_per_die = Q3N_PLANES_PER_DIE,
		.ecc_step_size = Q3N_ECC_STEP_SIZE,
		.ecc_strength = Q3N_ECC_STRENGTH,
		.ldpc_bytes_per_step = Q3N_LDPC_BYTES_PER_STEP,
		.ldpc_steps = Q3N_LDPC_STEPS,
	},
};

const struct q3n_device_desc *q3n_device_match(const u8 *id, size_t len)
{
	size_t index;

	if (!id)
		return NULL;
	for (index = 0; index < ARRAY_SIZE(q3n_devices); index++) {
		const struct q3n_device_desc *device = &q3n_devices[index];

		if (len == device->id_len && !memcmp(id, device->id, len))
			return device;
	}
	return NULL;
}

int q3n_device_validate(struct qemu_3dnand *q3n,
			const struct q3n_device_desc *device)
{
	u64 pool_blocks;

	if (!q3n || !device)
		return -EINVAL;
	pool_blocks = (u64)q3n->data_blocks_per_plane +
		q3n->parity_blocks_per_plane + q3n->metadata_blocks_per_plane +
		q3n->reserve_blocks_per_plane;
	if ((q3n->cap & device->required_caps) != device->required_caps)
		return -EINVAL;
	if (q3n->page_size != device->page_size ||
	    q3n->oob_size != device->logical_oob_size ||
	    q3n->pages_per_block != device->pages_per_block ||
	    q3n->blocks_per_plane != device->blocks_per_plane ||
	    !q3n->data_blocks_per_plane || !q3n->parity_blocks_per_plane ||
	    pool_blocks > device->blocks_per_plane)
		return -EINVAL;
	if (device->physical_oob_size != Q3N_PHYSICAL_OOB_SIZE ||
	    q3n->ecc_step_size != device->ecc_step_size ||
	    q3n->ecc_strength != device->ecc_strength ||
	    q3n->ldpc_bytes_per_step != device->ldpc_bytes_per_step ||
	    q3n->ldpc_steps != device->ldpc_steps)
		return -EINVAL;
	return 0;
}

int q3n_device_build_scan_id(struct qemu_3dnand *q3n,
			     struct nand_flash_dev *scan_id, u32 writesize,
			     u32 oobsize, u32 erasesize, u64 size)
{
	if (!q3n || !q3n->device || !scan_id || !writesize || !erasesize ||
	    !size || size > U64_MAX - SZ_1M)
		return -EINVAL;
	*scan_id = (struct nand_flash_dev) {
		.name = (char *)q3n_device_name(q3n->device),
		.id_len = q3n->nand_id_len,
		.pagesize = writesize,
		.oobsize = oobsize,
		.erasesize = erasesize,
		.chipsize = DIV_ROUND_UP_ULL(size, SZ_1M),
		.options = NAND_NO_SUBPAGE_WRITE | NAND_NON_POWER_OF_2_GEOMETRY |
			NAND_MARKBAD_NO_ERASE,
		.ecc = NAND_ECC_INFO(Q3N_ECC_STRENGTH, Q3N_ECC_STEP_SIZE),
	};
	memcpy(scan_id->id, q3n->nand_id, q3n->nand_id_len);
	return 0;
}

const char *q3n_device_name(const struct q3n_device_desc *device)
{
	return device ? device->name : "unknown";
}

u8 q3n_device_dies(const struct q3n_device_desc *device)
{
	return device ? device->dies : 0;
}

u8 q3n_device_planes_per_die(const struct q3n_device_desc *device)
{
	return device ? device->planes_per_die : 0;
}
