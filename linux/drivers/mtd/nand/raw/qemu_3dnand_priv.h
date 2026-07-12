/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QEMU_3DNAND_PRIV_H
#define __QEMU_3DNAND_PRIV_H

#include <linux/types.h>

#define Q3N_RAID_MAX_DATA_PAGES	7
#define Q3N_RAID_META_MAGIC		0x5133
#define Q3N_RAID_META_VERSION		1

struct q3n_geometry {
	u32 page_size;
	u32 pages_per_block;
	u32 data_pages_per_stripe;
	u32 data_block_count;
	u32 parity_block_count;
};

struct q3n_phys_addr {
	u32 block;
	u32 page;
};

struct q3n_block_state {
	u32 next_prog_page;
};

enum q3n_stripe_state {
	Q3N_STRIPE_EMPTY,
	Q3N_STRIPE_OPEN,
	Q3N_STRIPE_UNPROTECTED,
	Q3N_STRIPE_PARITY_QUEUED,
	Q3N_STRIPE_PROTECTED,
	Q3N_STRIPE_PARITY_FAILED,
};

struct q3n_data_meta {
	__le16 magic;
	u8 version;
	u8 slot;
	__le64 stripe_id;
	__le32 data_crc;
	__le32 header_crc;
} __packed;

struct q3n_parity_manifest {
	__le16 magic;
	u8 version;
	u8 data_pages;
	__le64 stripe_id;
	__le16 member_bitmap;
	__le32 data_crc[Q3N_RAID_MAX_DATA_PAGES];
	__le32 parity_crc;
	__le32 header_crc;
} __packed;

struct q3n_open_stripe {
	u64 stripe_id;
	u16 member_bitmap;
	u8 data_pages;
	u8 *parity;
	size_t page_size;
	u32 data_crc[Q3N_RAID_MAX_DATA_PAGES];
	enum q3n_stripe_state state;
};

int q3n_map_data_page(const struct q3n_geometry *geometry, u64 stripe,
		      u8 slot, struct q3n_phys_addr *out);
int q3n_map_parity_page(const struct q3n_geometry *geometry, u64 stripe,
			struct q3n_phys_addr *out);
bool q3n_program_order_ready(const struct q3n_block_state *state, u32 page);
void q3n_xor_page(u8 *parity, const u8 *data, size_t len);
int q3n_open_stripe_update(struct q3n_open_stripe *stripe, u8 slot,
			   const u8 *data, size_t len);
int q3n_open_stripe_queue_parity(struct q3n_open_stripe *stripe);
void q3n_open_stripe_complete_parity(struct q3n_open_stripe *stripe, bool ok);
int q3n_build_manifest(const struct q3n_open_stripe *stripe,
		       struct q3n_parity_manifest *manifest);
int q3n_validate_manifest(const struct q3n_parity_manifest *manifest);
int q3n_recover_page(u8 *out, const u8 *parity, const u8 * const *members,
		     u8 data_pages, u8 missing_slot, size_t len);

#endif /* __QEMU_3DNAND_PRIV_H */
