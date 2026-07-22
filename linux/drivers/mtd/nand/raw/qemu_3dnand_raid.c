// SPDX-License-Identifier: GPL-2.0

#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "qemu_3dnand.h"
#include "qemu_3dnand_priv.h"

static u16 q3n_member_mask(u8 data_pages)
{
	return GENMASK(data_pages - 1, 0);
}

static u32 q3n_manifest_header_crc(const struct q3n_parity_manifest *manifest)
{
	struct q3n_parity_manifest copy = *manifest;

	copy.header_crc = 0;
	return crc32_le(~0, (const u8 *)&copy, sizeof(copy));
}

static u32 q3n_data_meta_header_crc(const struct q3n_data_meta *meta)
{
	struct q3n_data_meta copy = *meta;

	copy.header_crc = 0;
	return crc32_le(~0, (const u8 *)&copy, sizeof(copy));
}

static u32 q3n_tombstone_header_crc(
		const struct q3n_unprotected_tombstone *tombstone)
{
	struct q3n_unprotected_tombstone copy = *tombstone;

	copy.header_crc = 0;
	return crc32_le(~0, (const u8 *)&copy, sizeof(copy));
}

void q3n_xor_page(u8 *parity, const u8 *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		parity[i] ^= data[i];
}

int q3n_open_stripe_update(struct q3n_open_stripe *stripe, u8 slot,
			   const u8 *data, size_t len)
{
	if (!stripe || !stripe->parity || !data || !stripe->page_size ||
	    stripe->data_pages == 0 || stripe->data_pages > Q3N_RAID_MAX_DATA_PAGES ||
	    slot >= stripe->data_pages || len != stripe->page_size)
		return -EINVAL;
	if (stripe->member_bitmap & BIT(slot))
		return -EEXIST;

	q3n_xor_page(stripe->parity, data, len);
	stripe->data_crc[slot] = crc32_le(~0, data, len);
	stripe->member_bitmap |= BIT(slot);
	stripe->state = Q3N_STRIPE_UNPROTECTED;
	return 0;
}

int q3n_open_stripe_queue_parity(struct q3n_open_stripe *stripe)
{
	if (!stripe)
		return -EINVAL;
	if (stripe->member_bitmap != q3n_member_mask(stripe->data_pages))
		return -EAGAIN;

	stripe->state = Q3N_STRIPE_PARITY_QUEUED;
	return 0;
}

void q3n_open_stripe_complete_parity(struct q3n_open_stripe *stripe, bool ok)
{
	if (!stripe || stripe->state != Q3N_STRIPE_PARITY_QUEUED)
		return;

	stripe->state = ok ? Q3N_STRIPE_PROTECTED : Q3N_STRIPE_UNPROTECTED;
}

int q3n_build_manifest(const struct q3n_open_stripe *stripe,
		       struct q3n_parity_manifest *manifest)
{
	u8 lane;

	if (!stripe || !manifest || !stripe->parity || !stripe->page_size ||
	    stripe->data_pages == 0 || stripe->data_pages > Q3N_RAID_MAX_DATA_PAGES ||
	    stripe->member_bitmap != q3n_member_mask(stripe->data_pages))
		return -EINVAL;

	memset(manifest, 0, sizeof(*manifest));
	manifest->magic = cpu_to_le16(Q3N_RAID_META_MAGIC);
	manifest->version = Q3N_RAID_META_VERSION;
	manifest->data_pages = stripe->data_pages;
	manifest->stripe_id = cpu_to_le64(stripe->stripe_id);
	manifest->member_bitmap = cpu_to_le16(stripe->member_bitmap);
	for (lane = 0; lane < Q3N_RAID_MAX_DATA_PAGES; lane++)
		manifest->data_crc[lane] = cpu_to_le32(stripe->data_crc[lane]);
	manifest->parity_crc = cpu_to_le32(crc32_le(~0, stripe->parity,
						     stripe->page_size));
	manifest->header_crc = cpu_to_le32(q3n_manifest_header_crc(manifest));
	return 0;
}

int q3n_validate_manifest(const struct q3n_parity_manifest *manifest)
{
	if (!manifest || le16_to_cpu(manifest->magic) != Q3N_RAID_META_MAGIC ||
	    manifest->version != Q3N_RAID_META_VERSION ||
	    manifest->data_pages == 0 ||
	    manifest->data_pages > Q3N_RAID_MAX_DATA_PAGES ||
	    le16_to_cpu(manifest->member_bitmap) !=
		q3n_member_mask(manifest->data_pages))
		return -EBADMSG;
	if (le32_to_cpu(manifest->header_crc) != q3n_manifest_header_crc(manifest))
		return -EBADMSG;

	return 0;
}

int q3n_pack_data_oob(u8 *logical_oob, size_t oob_len,
		      const struct q3n_data_meta *meta)
{
	struct q3n_data_meta encoded;

	if (!logical_oob || !meta || oob_len < Q3N_LOGICAL_OOB_SIZE)
		return -EINVAL;

	encoded = *meta;
	encoded.magic = cpu_to_le16(Q3N_RAID_META_MAGIC);
	encoded.version = Q3N_RAID_META_VERSION;
	encoded.header_crc = cpu_to_le32(q3n_data_meta_header_crc(&encoded));
	memset(logical_oob, 0xff, Q3N_LOGICAL_OOB_SIZE);
	memcpy(logical_oob + 1, &encoded, sizeof(encoded));
	return 0;
}

int q3n_unpack_data_oob(const u8 *logical_oob, size_t oob_len,
			struct q3n_data_meta *meta)
{
	if (!logical_oob || !meta || oob_len < Q3N_LOGICAL_OOB_SIZE)
		return -EINVAL;

	memcpy(meta, logical_oob + 1, sizeof(*meta));
	if (le16_to_cpu(meta->magic) != Q3N_RAID_META_MAGIC ||
	    meta->version != Q3N_RAID_META_VERSION ||
	    le32_to_cpu(meta->header_crc) != q3n_data_meta_header_crc(meta))
		return -EBADMSG;

	return 0;
}

int q3n_pack_parity_oob(u8 *logical_oob, size_t oob_len,
			const struct q3n_parity_manifest *manifest)
{
	int ret;

	if (!logical_oob || !manifest || oob_len < Q3N_LOGICAL_OOB_SIZE)
		return -EINVAL;
	ret = q3n_validate_manifest(manifest);
	if (ret)
		return ret;

	memset(logical_oob, 0xff, Q3N_LOGICAL_OOB_SIZE);
	memcpy(logical_oob + 1, manifest, sizeof(*manifest));
	return 0;
}

int q3n_unpack_parity_oob(const u8 *logical_oob, size_t oob_len,
			  struct q3n_parity_manifest *manifest)
{
	if (!logical_oob || !manifest || oob_len < Q3N_LOGICAL_OOB_SIZE)
		return -EINVAL;

	memcpy(manifest, logical_oob + 1, sizeof(*manifest));
	return q3n_validate_manifest(manifest);
}

int q3n_pack_unprotected_oob(u8 *logical_oob, size_t oob_len,
			     const struct q3n_unprotected_tombstone *tombstone)
{
	struct q3n_unprotected_tombstone encoded;

	if (!logical_oob || !tombstone || oob_len < Q3N_LOGICAL_OOB_SIZE ||
	    tombstone->reason < Q3N_UNPROTECTED_INVALID_METADATA ||
	    tombstone->reason > Q3N_UNPROTECTED_REBUILD)
		return -EINVAL;

	encoded = *tombstone;
	encoded.magic = cpu_to_le16(Q3N_RAID_TOMBSTONE_MAGIC);
	encoded.version = Q3N_RAID_META_VERSION;
	encoded.header_crc = cpu_to_le32(q3n_tombstone_header_crc(&encoded));
	memset(logical_oob, 0xff, Q3N_LOGICAL_OOB_SIZE);
	memcpy(logical_oob + 1, &encoded, sizeof(encoded));
	return 0;
}

int q3n_unpack_unprotected_oob(const u8 *logical_oob, size_t oob_len,
			       struct q3n_unprotected_tombstone *tombstone)
{
	if (!logical_oob || !tombstone || oob_len < Q3N_LOGICAL_OOB_SIZE)
		return -EINVAL;

	memcpy(tombstone, logical_oob + 1, sizeof(*tombstone));
	if (le16_to_cpu(tombstone->magic) != Q3N_RAID_TOMBSTONE_MAGIC ||
	    tombstone->version != Q3N_RAID_META_VERSION ||
	    tombstone->reason < Q3N_UNPROTECTED_INVALID_METADATA ||
	    tombstone->reason > Q3N_UNPROTECTED_REBUILD ||
	    le32_to_cpu(tombstone->header_crc) !=
		q3n_tombstone_header_crc(tombstone))
		return -EBADMSG;

	return 0;
}

int q3n_recover_page(u8 *out, const u8 *parity, const u8 * const *members,
		     u8 data_pages, u8 missing_slot, size_t len)
{
	u8 slot;

	if (!out || !parity || !members || !len || !data_pages ||
	    data_pages > Q3N_RAID_MAX_DATA_PAGES || missing_slot >= data_pages)
		return -EINVAL;

	memcpy(out, parity, len);
	for (slot = 0; slot < data_pages; slot++) {
		if (slot == missing_slot)
			continue;
		if (!members[slot])
			return -EINVAL;
		q3n_xor_page(out, members[slot], len);
	}

	return 0;
}

int q3n_rebuild_xor_one(struct q3n_parity_rebuild *rebuild,
			const u8 *member)
{
	if (!rebuild || !member || !rebuild->parity_accumulator ||
	    !rebuild->page_size || !rebuild->data_pages ||
	    rebuild->data_pages > Q3N_RAID_MAX_DATA_PAGES ||
	    rebuild->next_slot >= rebuild->data_pages)
		return -EINVAL;

	q3n_xor_page(rebuild->parity_accumulator, member, rebuild->page_size);
	rebuild->next_slot++;
	return 0;
}

int q3n_rebuild_check_generation(const struct q3n_parity_rebuild *rebuild,
				 u32 current_generation)
{
	if (!rebuild || !rebuild->generation || !current_generation)
		return -EINVAL;

	return rebuild->generation == current_generation ? 0 : -ESTALE;
}

MODULE_DESCRIPTION("QEMU 3D NAND serial Page-RAID metadata and XOR");
MODULE_LICENSE("GPL");
