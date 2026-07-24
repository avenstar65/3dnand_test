/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QEMU_3DNAND_PRIV_H
#define __QEMU_3DNAND_PRIV_H

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#define Q3N_RAID_MAX_DATA_PAGES	7
#define Q3N_RAID_META_MAGIC		0x5133
#define Q3N_RAID_META_VERSION		1
#define Q3N_MAX_PENDING_PARITY		32
#define Q3N_MAX_PARITY_READ_INFLIGHT	1

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

struct q3n_block_barrier {
	atomic_t pending_parity;
	bool cancelling;
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

struct q3n_parity_rebuild {
	u64 stripe_id;
	u32 generation;
	u16 member_bitmap;
	u8 missing_slot;
	u8 next_slot;
	u8 data_pages;
	u8 *parity_accumulator;
	size_t page_size;
	u32 data_crc[Q3N_RAID_MAX_DATA_PAGES];
};

struct q3n_ecc_result {
	u32 max_bitflips;
	u32 corrected_bits;
	u32 failed_step;
	bool uncorrectable;
};

static inline void q3n_ecc_accumulate(struct q3n_ecc_result *total,
				      const struct q3n_ecc_result *page)
{
	if (page->max_bitflips > total->max_bitflips)
		total->max_bitflips = page->max_bitflips;
	total->corrected_bits += page->corrected_bits;
	if (page->uncorrectable && !total->uncorrectable)
		total->failed_step = page->failed_step;
	total->uncorrectable |= page->uncorrectable;
}

static inline int
q3n_ecc_result_to_mtd_ret(const struct q3n_ecc_result *result)
{
	return result->uncorrectable ? -EBADMSG : result->max_bitflips;
}

enum q3n_req_class {
	Q3N_REQ_FOREGROUND,
	Q3N_REQ_PARITY_READ,
	Q3N_REQ_PARITY_WRITE,
};

enum q3n_req_op {
	Q3N_REQ_READ,
	Q3N_REQ_PROGRAM,
};

struct q3n_request {
	struct list_head node;
	enum q3n_req_class class;
	enum q3n_req_op op;
	u32 page;
};

struct q3n_sched {
	spinlock_t lock;
	wait_queue_head_t waitq;
	atomic64_t sequence;
	struct list_head foreground_queue;
	struct list_head parity_read_queue;
	struct list_head parity_write_queue;
	u32 pending_parity;
	u32 reserved_parity;
	u32 max_pending_parity;
	u64 p1_over_p2;
};

int q3n_map_data_page(const struct q3n_geometry *geometry, u64 stripe,
		      u8 slot, struct q3n_phys_addr *out);
int q3n_map_parity_page(const struct q3n_geometry *geometry, u64 stripe,
			struct q3n_phys_addr *out);
int q3n_map_serial_data_page(const struct q3n_geometry *geometry,
			     u64 logical_page, struct q3n_phys_addr *out);
void q3n_xor_page(u8 *parity, const u8 *data, size_t len);
int q3n_open_stripe_update(struct q3n_open_stripe *stripe, u8 slot,
			   const u8 *data, size_t len);
int q3n_open_stripe_queue_parity(struct q3n_open_stripe *stripe);
void q3n_open_stripe_complete_parity(struct q3n_open_stripe *stripe, bool ok);
int q3n_build_manifest(const struct q3n_open_stripe *stripe,
		       struct q3n_parity_manifest *manifest);
int q3n_validate_manifest(const struct q3n_parity_manifest *manifest);
int q3n_pack_data_oob(u8 *logical_oob, size_t oob_len,
		      const struct q3n_data_meta *meta);
int q3n_unpack_data_oob(const u8 *logical_oob, size_t oob_len,
			struct q3n_data_meta *meta);
int q3n_pack_parity_oob(u8 *logical_oob, size_t oob_len,
			const struct q3n_parity_manifest *manifest);
int q3n_unpack_parity_oob(const u8 *logical_oob, size_t oob_len,
			  struct q3n_parity_manifest *manifest);
int q3n_recover_page(u8 *out, const u8 *parity, const u8 * const *members,
		     u8 data_pages, u8 missing_slot, size_t len);
int q3n_rebuild_xor_one(struct q3n_parity_rebuild *rebuild,
			const u8 *member);
int q3n_rebuild_check_generation(const struct q3n_parity_rebuild *rebuild,
				 u32 current_generation);
void q3n_block_barrier_init(struct q3n_block_barrier *barrier);
int q3n_block_parity_get(struct q3n_block_barrier *barrier);
bool q3n_block_parity_put(struct q3n_block_barrier *barrier);
int q3n_block_cancel_begin(struct q3n_block_barrier *barrier);
void q3n_block_cancel_end(struct q3n_block_barrier *barrier);
bool q3n_block_is_cancelling(const struct q3n_block_barrier *barrier);
int q3n_block_pending(const struct q3n_block_barrier *barrier);
void q3n_sched_init(struct q3n_sched *sched);
int q3n_sched_enqueue(struct q3n_sched *sched, struct q3n_request *req);
int q3n_sched_cancel(struct q3n_sched *sched, struct q3n_request *req);
int q3n_sched_requeue_p1(struct q3n_sched *sched, struct q3n_request *req);
int q3n_sched_reserve_parity(struct q3n_sched *sched);
void q3n_sched_release_parity(struct q3n_sched *sched);
void q3n_sched_get_counts(struct q3n_sched *sched, u32 *pending,
			  u32 *reserved, u32 *max_pending);
u64 q3n_sched_get_p1_over_p2(struct q3n_sched *sched);
int q3n_sched_try_start(struct q3n_sched *sched, struct q3n_request *req);
int q3n_sched_try_start_seq(struct q3n_sched *sched, struct q3n_request *req,
			    u64 *sequence);
void q3n_sched_wait_for_change(struct q3n_sched *sched, u64 sequence);
void q3n_sched_notify(struct q3n_sched *sched);
struct q3n_request *q3n_sched_pick_next(struct q3n_sched *sched);
void q3n_sched_drain(struct q3n_sched *sched);

#endif /* __QEMU_3DNAND_PRIV_H */
