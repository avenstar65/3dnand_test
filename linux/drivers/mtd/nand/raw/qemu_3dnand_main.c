// SPDX-License-Identifier: GPL-2.0
/*
 * Linux MTD driver for the QEMU 3D NAND controller model.
 *
 * QEMU provides only physical flash/controller semantics. This driver owns the
 * same-block serial page-raid layout: seven data pages followed by one hidden
 * parity page (D0..D6,P) per stripe.
 */

#include <linux/bitops.h>
#include <linux/crc32.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mtd/mtd.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "qemu_3dnand.h"
#include "qemu_3dnand_priv.h"

#define Q3N_DATA_PAGES 7
#define Q3N_STRIPE_PAGES (Q3N_DATA_PAGES + 1)
#define Q3N_RAID_LANES Q3N_DATA_PAGES

struct qemu_3dnand_data_block_meta {
	u32 generation;
	bool bad;
	bool erased;
	struct q3n_block_barrier parity_barrier;
};

struct qemu_3dnand_parity_entry {
	u32 physical_block;
	u32 page;
	u32 data_block_generation[Q3N_RAID_LANES];
	u32 data_crc[Q3N_RAID_LANES];
	u32 parity_crc;
	u32 parity_version;
	u64 sequence;
	bool valid;
};

struct qemu_3dnand {
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_size;
	struct mutex mtd_lock;
	struct q3n_sched sched;
	struct workqueue_struct *parity_wq;
	wait_queue_head_t parity_cancel_waitq;
	wait_queue_head_t parity_pause_waitq;
	atomic_t parity_paused;
	atomic_t parity_continuation_paused;
	atomic_t fail_next_parity_queue;
	u32 parity_pause_block;
	u32 parity_continuation_pause_block;
	u32 parity_continuation_pause_class;
	bool parity_pause_enable;
	bool parity_continuation_pause_enable;
	struct dentry *debugfs_dir;
	struct mtd_info mtd;

	u8 *page_buf;
	u8 *raid_buf;
	u8 *data_page_valid;
	struct q3n_block_state *program_state;
	struct qemu_3dnand_data_block_meta *data_meta;
	struct qemu_3dnand_parity_entry *parity_index;

	u32 page_size;
	u32 oob_size;
	u32 pages_per_block;
	u32 blocks_per_plane;
	u32 data_blocks_per_plane;
	u32 parity_blocks_per_plane;
	u32 metadata_blocks_per_plane;
	u32 reserve_blocks_per_plane;
	u32 cap;
	u32 data_block_count;
	u32 parity_block_count;
	u32 raid_group_count;
	u64 parity_next_page;
	u64 parity_sequence;

	u64 parity_written;
	u64 parity_stale;
	u64 raid_recovered;
	u64 raid_failed;
	u64 generation_updates;
	atomic64_t protected_stripes;
	atomic64_t unprotected_stripes;
	atomic64_t failed_stripes;
};

struct qemu_3dnand_parity_work {
	struct work_struct work;
	struct qemu_3dnand *q3n;
	struct q3n_request request;
	struct q3n_parity_rebuild rebuild;
	u32 block;
	u32 stripe;
	u8 *page_buf;
	u8 tombstone_reason;
	bool tombstone;
	bool request_queued;
};

static void qemu_3dnand_finish_parity_work(
		struct qemu_3dnand_parity_work *parity)
{
	struct qemu_3dnand *q3n = parity->q3n;
	struct q3n_block_barrier *barrier =
		&q3n->data_meta[parity->block].parity_barrier;

	q3n_sched_release_parity(&q3n->sched);
	if (q3n_block_parity_put(barrier))
		wake_up_all(&q3n->parity_cancel_waitq);
	kfree(parity->page_buf);
	kfree(parity->rebuild.parity_accumulator);
	kfree(parity);
}

static void qemu_3dnand_free_metadata(struct qemu_3dnand *q3n)
{
	kvfree(q3n->data_page_valid);
	q3n->data_page_valid = NULL;
	kvfree(q3n->parity_index);
	q3n->parity_index = NULL;
}

static int qemu_3dnand_lock_request(struct qemu_3dnand *q3n,
				    struct q3n_request *req)
{
	u64 sequence;
	int ret;

	ret = q3n_sched_enqueue(&q3n->sched, req);
	if (ret)
		return ret;

	for (;;) {
		mutex_lock(&q3n->mtd_lock);
		ret = q3n_sched_try_start_seq(&q3n->sched, req, &sequence);
		if (!ret)
			return 0;
		mutex_unlock(&q3n->mtd_lock);
		if (ret != -EAGAIN)
			return ret;
		q3n_sched_wait_for_change(&q3n->sched, sequence);
	}
}

static int qemu_3dnand_reserve_parity(struct qemu_3dnand *q3n)
{
	int ret;

	ret = q3n_sched_reserve_parity(&q3n->sched);
	if (ret != -ENOSPC)
		return ret;

	flush_workqueue(q3n->parity_wq);
	return q3n_sched_reserve_parity(&q3n->sched);
}

static u32 qemu_3dnand_readl(struct qemu_3dnand *q3n, u32 reg)
{
	return readl(q3n->regs + reg);
}

static void qemu_3dnand_writel(struct qemu_3dnand *q3n, u32 reg, u32 val)
{
	writel(val, q3n->regs + reg);
}

static int qemu_3dnand_wait_ready(struct qemu_3dnand *q3n)
{
	u32 status = qemu_3dnand_readl(q3n, Q3N_REG_STATUS);

	if (!(status & Q3N_STATUS_READY))
		return -ETIMEDOUT;

	if (status & Q3N_STATUS_ERROR)
		return -EIO;

	return 0;
}

static loff_t qemu_3dnand_phys_addr(struct qemu_3dnand *q3n, u32 block,
				    u32 page)
{
	return ((loff_t)block * q3n->pages_per_block + page) * q3n->page_size;
}

static void qemu_3dnand_set_addr(struct qemu_3dnand *q3n, loff_t addr)
{
	qemu_3dnand_writel(q3n, Q3N_REG_ADDR_LO, lower_32_bits(addr));
	qemu_3dnand_writel(q3n, Q3N_REG_ADDR_HI, upper_32_bits(addr));
}

static int qemu_3dnand_read_phys_page_locked(struct qemu_3dnand *q3n,
					     u32 block, u32 page, u8 *buf,
					     u32 op_class)
{
	u32 *words = (u32 *)buf;
	int ret;
	u32 i;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, page));
	qemu_3dnand_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	qemu_3dnand_writel(q3n, Q3N_REG_LEN, q3n->page_size);
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE);
	ret = qemu_3dnand_wait_ready(q3n);
	if (ret)
		return ret;

	for (i = 0; i < q3n->page_size / sizeof(u32); i++)
		words[i] = qemu_3dnand_readl(q3n, Q3N_REG_DATA);

	return 0;
}

static int qemu_3dnand_read_phys_page_oob_locked(
		struct qemu_3dnand *q3n, u32 block, u32 page, u8 *data,
		u8 *logical_oob, u32 op_class, struct q3n_ecc_result *ecc)
{
	int ret;
	u32 i;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, page));
	qemu_3dnand_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	qemu_3dnand_writel(q3n, Q3N_REG_LEN, q3n->page_size);
	qemu_3dnand_writel(q3n, Q3N_REG_OOB_LEN, Q3N_LOGICAL_OOB_SIZE);
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE_OOB);
	ret = qemu_3dnand_wait_ready(q3n);
	if (ret)
		return ret;

	for (i = 0; i < q3n->page_size; i += sizeof(u32))
		put_unaligned_le32(qemu_3dnand_readl(q3n, Q3N_REG_DATA),
				   data + i);
	for (i = 0; i < Q3N_LOGICAL_OOB_SIZE; i += sizeof(u32))
		put_unaligned_le32(qemu_3dnand_readl(q3n, Q3N_REG_DATA),
				   logical_oob + i);

	if (ecc) {
		ecc->status = qemu_3dnand_readl(q3n, Q3N_REG_ECC_STATUS);
		ecc->max_bitflips = qemu_3dnand_readl(q3n,
						      Q3N_REG_ECC_MAX_BITFLIPS);
		ecc->corrected_bits = qemu_3dnand_readl(q3n,
						       Q3N_REG_ECC_CORRECTED_BITS);
		ecc->failed_step = qemu_3dnand_readl(q3n,
						     Q3N_REG_ECC_FAILED_STEP);
	}

	return 0;
}

static int qemu_3dnand_program_phys_page_oob_locked(
		struct qemu_3dnand *q3n, u32 block, u32 page, const u8 *data,
		const u8 *logical_oob, u32 op_class)
{
	int ret;
	u32 i;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, page));
	qemu_3dnand_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	qemu_3dnand_writel(q3n, Q3N_REG_LEN, q3n->page_size);
	qemu_3dnand_writel(q3n, Q3N_REG_OOB_LEN, Q3N_LOGICAL_OOB_SIZE);
	for (i = 0; i < q3n->page_size; i += sizeof(u32))
		qemu_3dnand_writel(q3n, Q3N_REG_DATA,
				     get_unaligned_le32(data + i));
	for (i = 0; i < Q3N_LOGICAL_OOB_SIZE; i += sizeof(u32))
		qemu_3dnand_writel(q3n, Q3N_REG_DATA,
				     get_unaligned_le32(logical_oob + i));

	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_PROGRAM_PAGE_OOB);
	ret = qemu_3dnand_wait_ready(q3n);
	if (!ret && block < q3n->data_block_count) {
		q3n->program_state[block].next_prog_page = page + 1;
		q3n_sched_notify(&q3n->sched);
	}
	return ret;
}

static int qemu_3dnand_erase_phys_block_locked(struct qemu_3dnand *q3n,
					       u32 block)
{
	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, 0));
	qemu_3dnand_writel(q3n, Q3N_REG_LEN,
			   q3n->pages_per_block * q3n->page_size);
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_ERASE_BLOCK);
	return qemu_3dnand_wait_ready(q3n);
}

static int qemu_3dnand_mark_phys_block_bad_locked(struct qemu_3dnand *q3n,
						   u32 block)
{
	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, 0));
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_MARK_BAD_BLOCK);
	return qemu_3dnand_wait_ready(q3n);
}

static int qemu_3dnand_get_phys_block_status_locked(
		struct qemu_3dnand *q3n, u32 block, u32 *status, u32 *next_page)
{
	int ret;

	if (block >= q3n->data_block_count || !status || !next_page)
		return -EINVAL;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, 0));
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_GET_BLOCK_STATUS);
	ret = qemu_3dnand_wait_ready(q3n);
	if (ret)
		return ret;

	*status = qemu_3dnand_readl(q3n, Q3N_REG_BLOCK_STATUS);
	*next_page = qemu_3dnand_readl(q3n, Q3N_REG_BLOCK_NEXT_PAGE);
	return 0;
}

static void qemu_3dnand_decode_logical(struct qemu_3dnand *q3n, loff_t addr,
				       u32 *data_block, u32 *page,
				       u32 *column)
{
	u64 logical_page = addr / q3n->page_size;
	u64 pages_per_block = (q3n->pages_per_block / Q3N_STRIPE_PAGES) *
		Q3N_DATA_PAGES;
	u64 block;
	u64 page_in_block;
	u32 stripe;
	u32 slot;

	*column = addr % q3n->page_size;
	block = div64_u64_rem(logical_page, pages_per_block, &page_in_block);
	stripe = div_u64_rem(page_in_block, Q3N_DATA_PAGES, &slot);
	*data_block = block;
	*page = stripe * Q3N_STRIPE_PAGES + slot;
}

static u32 qemu_3dnand_data_page_index(struct qemu_3dnand *q3n,
				       u32 data_block, u32 page)
{
	return data_block * q3n->pages_per_block + page;
}

static u32 qemu_3dnand_parity_index(struct qemu_3dnand *q3n, u32 group,
				    u32 page)
{
	return group * (q3n->pages_per_block / Q3N_STRIPE_PAGES) + page;
}

static u64 qemu_3dnand_stripe_id(struct qemu_3dnand *q3n, u32 block,
				  u32 stripe)
{
	return (u64)block * q3n->pages_per_block + stripe;
}

static int qemu_3dnand_validate_data_metadata(
		const u8 *data, size_t data_len, const u8 *logical_oob,
		u64 stripe_id, u8 slot, u32 *data_crc)
{
	struct q3n_data_meta meta;
	u32 crc;
	int ret;

	ret = q3n_unpack_data_oob(logical_oob, Q3N_LOGICAL_OOB_SIZE, &meta);
	if (ret)
		return ret;
	crc = crc32_le(~0, data, data_len);
	if (le64_to_cpu(meta.stripe_id) != stripe_id || meta.slot != slot ||
	    le32_to_cpu(meta.data_crc) != crc)
		return -EBADMSG;

	if (data_crc)
		*data_crc = crc;
	return 0;
}

static int qemu_3dnand_validate_replay_members_locked(
		struct qemu_3dnand *q3n, u32 block, u32 stripe,
		const struct q3n_parity_manifest *manifest)
{
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	struct q3n_ecc_result ecc;
	u32 data_crc;
	u32 lane;
	int ret;

	for (lane = 0; lane < Q3N_DATA_PAGES; lane++) {
		ret = qemu_3dnand_read_phys_page_oob_locked(q3n, block,
			stripe * Q3N_STRIPE_PAGES + lane, q3n->page_buf,
			logical_oob, Q3N_OP_PARITY_READ, &ecc);
		if (ret)
			return ret;
		ret = qemu_3dnand_validate_data_metadata(q3n->page_buf,
			q3n->page_size, logical_oob,
			qemu_3dnand_stripe_id(q3n, block, stripe), lane,
			&data_crc);
		if (ret)
			return ret;
		if (le32_to_cpu(manifest->data_crc[lane]) != data_crc)
			return -EBADMSG;
	}

	return 0;
}

static bool qemu_3dnand_stripe_full(struct qemu_3dnand *q3n, u32 block,
				    u32 stripe)
{
	u32 lane;

	if (block >= q3n->data_block_count)
		return false;

	for (lane = 0; lane < Q3N_DATA_PAGES; lane++) {
		if (!q3n->data_page_valid[qemu_3dnand_data_page_index(q3n,
							       block,
							       stripe * Q3N_STRIPE_PAGES + lane)])
			return false;
	}

	return true;
}

static bool qemu_3dnand_parity_generation_valid(struct qemu_3dnand *q3n,
						struct qemu_3dnand_parity_entry *entry,
						u32 block)
{
	return entry->valid && block < q3n->data_block_count &&
		entry->data_block_generation[0] == q3n->data_meta[block].generation;
}

static void qemu_3dnand_mark_parity_stale(struct qemu_3dnand *q3n,
					  struct qemu_3dnand_parity_entry *entry)
{
	if (!entry->valid)
		return;

	entry->valid = false;
	q3n->parity_stale++;
}

static int qemu_3dnand_commit_parity_locked(struct qemu_3dnand *q3n,
					    u32 block, u32 stripe,
					    struct qemu_3dnand_parity_entry *entry,
					    const u8 *parity,
					    const u32 *data_crc)
{
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	struct q3n_open_stripe open_stripe = {
		.stripe_id = qemu_3dnand_stripe_id(q3n, block, stripe),
		.member_bitmap = GENMASK(Q3N_DATA_PAGES - 1, 0),
		.data_pages = Q3N_DATA_PAGES,
		.parity = (u8 *)parity,
		.page_size = q3n->page_size,
	};
	struct q3n_parity_manifest manifest;
	int ret;

	memcpy(open_stripe.data_crc, data_crc, sizeof(open_stripe.data_crc));
	ret = q3n_build_manifest(&open_stripe, &manifest);
	if (ret)
		return ret;
	ret = q3n_pack_parity_oob(logical_oob, sizeof(logical_oob), &manifest);
	if (ret)
		return ret;
	ret = qemu_3dnand_program_phys_page_oob_locked(q3n, block,
			stripe * Q3N_STRIPE_PAGES + Q3N_DATA_PAGES, parity,
			logical_oob, Q3N_OP_PARITY_WRITE);
	if (ret)
		return ret;

	entry->physical_block = block;
	entry->page = stripe * Q3N_STRIPE_PAGES + Q3N_DATA_PAGES;
	entry->data_block_generation[0] = q3n->data_meta[block].generation;
	memcpy(entry->data_crc, data_crc, sizeof(entry->data_crc));
	entry->parity_crc = le32_to_cpu(manifest.parity_crc);
	entry->parity_version++;
	entry->sequence = ++q3n->parity_sequence;
	entry->valid = true;
	q3n->parity_written++;
	atomic64_inc(&q3n->protected_stripes);
	return 0;
}

static int qemu_3dnand_program_unprotected_tombstone_locked(
		struct qemu_3dnand *q3n, u32 block, u32 stripe, u8 reason,
		u8 *page_buf)
{
	struct q3n_unprotected_tombstone tombstone = {
		.reason = reason,
		.stripe_id = cpu_to_le64(qemu_3dnand_stripe_id(q3n, block,
								 stripe)),
	};
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	int ret;

	if (!page_buf)
		return -EINVAL;
	ret = q3n_pack_unprotected_oob(logical_oob, sizeof(logical_oob),
					       &tombstone);
	if (ret)
		return ret;
	memset(page_buf, 0xff, q3n->page_size);
	return qemu_3dnand_program_phys_page_oob_locked(q3n, block,
		stripe * Q3N_STRIPE_PAGES + Q3N_DATA_PAGES, page_buf,
		logical_oob, Q3N_OP_PARITY_WRITE);
}

static int qemu_3dnand_append_parity_locked(struct qemu_3dnand *q3n,
					    u32 block, u32 stripe,
					    u8 *failure_reason)
{
	struct qemu_3dnand_parity_entry *entry;
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	u32 data_crc[Q3N_DATA_PAGES];
	struct q3n_ecc_result ecc;
	u32 lane;
	u32 i;
	int ret;

	if (!failure_reason)
		return -EINVAL;
	*failure_reason = Q3N_UNPROTECTED_INVALID_METADATA;
	if (!qemu_3dnand_stripe_full(q3n, block, stripe))
		return 0;

	entry = &q3n->parity_index[qemu_3dnand_parity_index(q3n, block, stripe)];
	memset(q3n->raid_buf, 0, q3n->page_size);
	for (lane = 0; lane < Q3N_DATA_PAGES; lane++) {
		*failure_reason = Q3N_UNPROTECTED_MEMBER_READ;
		ret = qemu_3dnand_read_phys_page_oob_locked(q3n, block,
				 stripe * Q3N_STRIPE_PAGES + lane,
						q3n->page_buf, logical_oob,
						Q3N_OP_PARITY_READ, &ecc);
		if (ret)
			return ret;
		if (ecc.status & Q3N_ECC_STATUS_UNCORRECTABLE) {
			*failure_reason = Q3N_UNPROTECTED_LDPC_UNCORRECTABLE;
			return -EBADMSG;
		}
		*failure_reason = Q3N_UNPROTECTED_INVALID_METADATA;
		ret = qemu_3dnand_validate_data_metadata(q3n->page_buf,
			q3n->page_size, logical_oob,
			qemu_3dnand_stripe_id(q3n, block, stripe), lane,
			&data_crc[lane]);
		if (ret)
			return ret;
		for (i = 0; i < q3n->page_size; i++)
			q3n->raid_buf[i] ^= q3n->page_buf[i];
	}

	*failure_reason = Q3N_UNPROTECTED_PARITY_PROGRAM;
	return qemu_3dnand_commit_parity_locked(q3n, block, stripe, entry,
						q3n->raid_buf, data_crc);
}

static int qemu_3dnand_restore_media_locked(struct qemu_3dnand *q3n)
{
	u32 stripes_per_block = q3n->pages_per_block / Q3N_STRIPE_PAGES;
	u8 *parity_valid;
	u32 block;
	int ret = 0;

	parity_valid = kcalloc(stripes_per_block, sizeof(*parity_valid),
			       GFP_KERNEL);
	if (!parity_valid)
		return -ENOMEM;

	for (block = 0; block < q3n->data_block_count; block++) {
		struct qemu_3dnand_data_block_meta *meta =
			&q3n->data_meta[block];
		u8 *data_valid = &q3n->data_page_valid[
			qemu_3dnand_data_page_index(q3n, block, 0)];
		u32 status;
		u32 next_page;
		u32 stripe;
		bool needs_tail_parity;

		ret = qemu_3dnand_get_phys_block_status_locked(q3n, block,
							     &status, &next_page);
		if (ret)
			break;

		meta->bad = status & Q3N_BLOCK_STATUS_BAD;
		meta->erased = status & Q3N_BLOCK_STATUS_ERASED;
		q3n->program_state[block].next_prog_page = next_page;
		if (meta->bad)
			continue;

		ret = q3n_replay_serial_frontier(q3n->pages_per_block,
						 next_page, data_valid,
						 parity_valid,
						 &needs_tail_parity);
		if (ret)
			break;

		for (stripe = 0; stripe < stripes_per_block; stripe++) {
			struct qemu_3dnand_parity_entry *entry;
			u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
			struct q3n_parity_manifest manifest;
			struct q3n_unprotected_tombstone tombstone;
			struct q3n_ecc_result ecc;
			u32 lane;

			if (!parity_valid[stripe])
				continue;
			entry = &q3n->parity_index[
				qemu_3dnand_parity_index(q3n, block, stripe)];
			ret = qemu_3dnand_read_phys_page_oob_locked(q3n, block,
				stripe * Q3N_STRIPE_PAGES + Q3N_DATA_PAGES,
				q3n->page_buf, logical_oob, Q3N_OP_PARITY_READ,
				&ecc);
			if (ret)
				break;
			if (get_unaligned_le16(logical_oob + 1) ==
			    Q3N_RAID_TOMBSTONE_MAGIC) {
				ret = q3n_unpack_unprotected_oob(logical_oob,
							 sizeof(logical_oob),
							 &tombstone);
				if (!ret && le64_to_cpu(tombstone.stripe_id) !=
				    qemu_3dnand_stripe_id(q3n, block, stripe))
					ret = -EBADMSG;
				entry->valid = false;
				atomic64_inc(&q3n->failed_stripes);
				atomic64_inc(&q3n->unprotected_stripes);
				if (ret)
					q3n->raid_failed++;
				ret = 0;
				continue;
			}
			ret = q3n_unpack_parity_oob(logical_oob,
						    sizeof(logical_oob), &manifest);
			if (!ret && manifest.data_pages != Q3N_DATA_PAGES)
				ret = -EBADMSG;
			if (!ret && le64_to_cpu(manifest.stripe_id) !=
				qemu_3dnand_stripe_id(q3n, block, stripe))
				ret = -EBADMSG;
			if (!ret && le32_to_cpu(manifest.parity_crc) !=
				crc32_le(~0, q3n->page_buf, q3n->page_size))
				ret = -EBADMSG;
			if (!ret)
				ret = qemu_3dnand_validate_replay_members_locked(
					q3n, block, stripe, &manifest);
			if (ret) {
				entry->valid = false;
				q3n->raid_failed++;
				atomic64_inc(&q3n->failed_stripes);
				atomic64_inc(&q3n->unprotected_stripes);
				ret = 0;
				continue;
			}
			entry->physical_block = block;
			entry->page = stripe * Q3N_STRIPE_PAGES +
				      Q3N_DATA_PAGES;
			entry->data_block_generation[0] = meta->generation;
			for (lane = 0; lane < Q3N_DATA_PAGES; lane++)
				entry->data_crc[lane] =
					le32_to_cpu(manifest.data_crc[lane]);
			entry->parity_crc = le32_to_cpu(manifest.parity_crc);
			entry->parity_version = 1;
			entry->sequence = ++q3n->parity_sequence;
			entry->valid = true;
		}
		if (ret)
			break;

		if (needs_tail_parity) {
			u32 restored_status;
			u32 restored_next;
			u8 failure_reason;

			stripe = next_page / Q3N_STRIPE_PAGES;
			ret = qemu_3dnand_append_parity_locked(q3n, block,
							  stripe,
							  &failure_reason);
			if (ret) {
				ret = qemu_3dnand_program_unprotected_tombstone_locked(
					q3n, block, stripe, failure_reason,
					q3n->page_buf);
				if (ret)
					break;
				q3n->raid_failed++;
				atomic64_inc(&q3n->failed_stripes);
				atomic64_inc(&q3n->unprotected_stripes);
			}
			ret = qemu_3dnand_get_phys_block_status_locked(
				q3n, block, &restored_status, &restored_next);
			if (ret)
				break;
			if (restored_next != next_page + 1) {
				ret = -EIO;
				break;
			}
		}
	}

	kfree(parity_valid);
	return ret;
}

static void qemu_3dnand_parity_worker(struct work_struct *work)
{
	struct qemu_3dnand_parity_work *parity =
		container_of(work, struct qemu_3dnand_parity_work, work);
	struct qemu_3dnand *q3n = parity->q3n;
	struct q3n_block_barrier *barrier =
		&q3n->data_meta[parity->block].parity_barrier;
	struct qemu_3dnand_parity_entry *entry;
	u64 sequence;
	int ret;

	if (READ_ONCE(q3n->parity_pause_enable) &&
	    READ_ONCE(q3n->parity_pause_block) == parity->block &&
	    !q3n_block_is_cancelling(barrier)) {
		atomic_inc(&q3n->parity_paused);
		wait_event(q3n->parity_pause_waitq,
			   !READ_ONCE(q3n->parity_pause_enable) ||
			   READ_ONCE(q3n->parity_pause_block) != parity->block ||
			   q3n_block_is_cancelling(barrier));
		atomic_dec(&q3n->parity_paused);
	}

again:
	if (!parity->request_queued) {
		ret = q3n_sched_requeue_p1(&q3n->sched,
					   &parity->request);
		if (ret)
			goto out_failed;
		parity->request_queued = true;
	}

	mutex_lock(&q3n->mtd_lock);
	if (q3n_block_is_cancelling(barrier)) {
		if (parity->request_queued)
			q3n_sched_cancel(&q3n->sched, &parity->request);
		parity->request_queued = false;
		mutex_unlock(&q3n->mtd_lock);
		qemu_3dnand_finish_parity_work(parity);
		return;
	}
	ret = q3n_rebuild_check_generation(&parity->rebuild,
		parity->q3n->data_meta[parity->block].generation);
	if (ret) {
		if (parity->request_queued)
			q3n_sched_cancel(&parity->q3n->sched, &parity->request);
		parity->request_queued = false;
		mutex_unlock(&parity->q3n->mtd_lock);
		if (ret == -ESTALE)
			parity->q3n->parity_stale++;
		else {
			parity->q3n->raid_failed++;
			atomic64_inc(&parity->q3n->failed_stripes);
		}
		goto out_finish;
	}
	ret = q3n_sched_try_start_seq(&parity->q3n->sched,
				      &parity->request, &sequence);
	if (ret == -EAGAIN) {
		mutex_unlock(&parity->q3n->mtd_lock);
		q3n_sched_wait_for_change(&parity->q3n->sched, sequence);
		goto again;
	}
	if (ret) {
		mutex_unlock(&parity->q3n->mtd_lock);
		goto out_failed;
	}
	parity->request_queued = false;

	if (parity->tombstone) {
		entry = &parity->q3n->parity_index[
			qemu_3dnand_parity_index(parity->q3n, parity->block,
						  parity->stripe)];
		ret = qemu_3dnand_program_unprotected_tombstone_locked(
			parity->q3n, parity->block, parity->stripe,
			parity->tombstone_reason, parity->page_buf);
		if (!ret)
			entry->valid = false;
	} else if (parity->rebuild.next_slot < Q3N_DATA_PAGES) {
		u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
		struct q3n_ecc_result ecc;
		u8 slot = parity->rebuild.next_slot;
		u8 reason = Q3N_UNPROTECTED_MEMBER_READ;

		ret = qemu_3dnand_read_phys_page_oob_locked(parity->q3n,
			parity->block,
			parity->stripe * Q3N_STRIPE_PAGES +
				slot,
			parity->page_buf, logical_oob, Q3N_OP_PARITY_READ, &ecc);
		if (!ret && (ecc.status & Q3N_ECC_STATUS_UNCORRECTABLE)) {
			reason = Q3N_UNPROTECTED_LDPC_UNCORRECTABLE;
			ret = -EBADMSG;
		}
		if (!ret) {
			reason = Q3N_UNPROTECTED_INVALID_METADATA;
			ret = qemu_3dnand_validate_data_metadata(
				parity->page_buf, parity->q3n->page_size,
				logical_oob, parity->rebuild.stripe_id, slot,
				&parity->rebuild.data_crc[slot]);
		}
		if (!ret) {
			reason = Q3N_UNPROTECTED_REBUILD;
			ret = q3n_rebuild_xor_one(&parity->rebuild,
						  parity->page_buf);
		}
		if (!ret && parity->rebuild.next_slot < Q3N_DATA_PAGES) {
			ret = q3n_sched_requeue_p1(&parity->q3n->sched,
						   &parity->request);
			parity->request_queued = !ret;
		} else if (!ret) {
			parity->request.class = Q3N_REQ_PARITY_WRITE;
			parity->request.op = Q3N_REQ_PROGRAM;
			ret = q3n_sched_enqueue(&parity->q3n->sched,
						&parity->request);
			parity->request_queued = !ret;
		} else if (ret) {
			parity->tombstone = true;
			parity->tombstone_reason = reason;
			parity->request.class = Q3N_REQ_PARITY_WRITE;
			parity->request.op = Q3N_REQ_PROGRAM;
			ret = q3n_sched_enqueue(&parity->q3n->sched,
						&parity->request);
			parity->request_queued = !ret;
		}
	} else {
		entry = &parity->q3n->parity_index[
			qemu_3dnand_parity_index(parity->q3n, parity->block,
						  parity->stripe)];
		ret = qemu_3dnand_commit_parity_locked(parity->q3n,
			parity->block, parity->stripe, entry,
			parity->rebuild.parity_accumulator,
			parity->rebuild.data_crc);
	}
	mutex_unlock(&parity->q3n->mtd_lock);
	if (ret)
		goto out_failed;
	if (parity->tombstone && !parity->request_queued) {
		parity->q3n->raid_failed++;
		atomic64_inc(&parity->q3n->failed_stripes);
		goto out_finish;
	}
	if (parity->rebuild.next_slot == Q3N_DATA_PAGES &&
	    !parity->request_queued)
		goto out_finish;
	if (READ_ONCE(q3n->parity_continuation_pause_enable) &&
	    READ_ONCE(q3n->parity_continuation_pause_block) == parity->block &&
	    READ_ONCE(q3n->parity_continuation_pause_class) ==
		parity->request.class &&
	    !q3n_block_is_cancelling(barrier)) {
		atomic_inc(&q3n->parity_continuation_paused);
		wait_event(q3n->parity_pause_waitq,
			   !READ_ONCE(q3n->parity_continuation_pause_enable) ||
			   READ_ONCE(q3n->parity_continuation_pause_block) !=
				parity->block ||
			   READ_ONCE(q3n->parity_continuation_pause_class) !=
				parity->request.class ||
			   q3n_block_is_cancelling(barrier));
		atomic_dec(&q3n->parity_continuation_paused);
	}
	cond_resched();
	goto again;

out_failed:
	parity->q3n->raid_failed++;
	atomic64_inc(&parity->q3n->failed_stripes);
out_finish:
	qemu_3dnand_finish_parity_work(parity);
}

static int qemu_3dnand_queue_parity_locked(struct qemu_3dnand *q3n,
					    u32 block, u32 stripe)
{
	struct qemu_3dnand_parity_work *parity;
	struct q3n_block_barrier *barrier =
		&q3n->data_meta[block].parity_barrier;
	int ret;

	if (atomic_xchg(&q3n->fail_next_parity_queue, 0)) {
		q3n_sched_release_parity(&q3n->sched);
		return -ENOMEM;
	}

	if (!qemu_3dnand_stripe_full(q3n, block, stripe)) {
		q3n_sched_release_parity(&q3n->sched);
		return -EINVAL;
	}
	ret = q3n_block_parity_get(barrier);
	if (ret) {
		q3n_sched_release_parity(&q3n->sched);
		return ret;
	}
	parity = kzalloc(sizeof(*parity), GFP_KERNEL);
	if (!parity) {
		q3n_sched_release_parity(&q3n->sched);
		if (q3n_block_parity_put(barrier))
			wake_up_all(&q3n->parity_cancel_waitq);
		return -ENOMEM;
	}
	parity->q3n = q3n;
	parity->block = block;
	parity->rebuild.parity_accumulator = kzalloc(q3n->page_size, GFP_KERNEL);
	parity->page_buf = kmalloc(q3n->page_size, GFP_KERNEL);
	if (!parity->rebuild.parity_accumulator || !parity->page_buf) {
		qemu_3dnand_finish_parity_work(parity);
		return -ENOMEM;
	}
	parity->stripe = stripe;
	parity->rebuild.stripe_id = qemu_3dnand_stripe_id(q3n, block, stripe);
	parity->rebuild.generation = q3n->data_meta[block].generation;
	parity->rebuild.data_pages = Q3N_DATA_PAGES;
	parity->rebuild.page_size = q3n->page_size;
	parity->request.class = Q3N_REQ_PARITY_READ;
	parity->request.op = Q3N_REQ_READ;
	parity->request.block_state = &q3n->program_state[block];
	parity->request.page = stripe * Q3N_STRIPE_PAGES + Q3N_DATA_PAGES;
	INIT_WORK(&parity->work, qemu_3dnand_parity_worker);
	if (q3n_sched_requeue_p1(&q3n->sched, &parity->request)) {
		qemu_3dnand_finish_parity_work(parity);
		return -EIO;
	}
	parity->request_queued = true;
	if (!queue_work(q3n->parity_wq, &parity->work)) {
		q3n_sched_cancel(&q3n->sched, &parity->request);
		parity->request_queued = false;
		qemu_3dnand_finish_parity_work(parity);
		return -EIO;
	}
	return 0;
}

static int qemu_3dnand_recover_page_locked(struct qemu_3dnand *q3n,
					   u32 data_block, u32 page, u8 *buf)
{
	u32 stripe = page / Q3N_STRIPE_PAGES;
	u32 missing_lane = page % Q3N_STRIPE_PAGES;
	struct qemu_3dnand_parity_entry *entry;
	u32 lane;
	u32 i;
	int ret;

	if (missing_lane >= Q3N_DATA_PAGES)
		return -EIO;

	entry = &q3n->parity_index[qemu_3dnand_parity_index(q3n, data_block, stripe)];
	if (!qemu_3dnand_parity_generation_valid(q3n, entry, data_block)) {
		qemu_3dnand_mark_parity_stale(q3n, entry);
		return -EIO;
	}

	ret = qemu_3dnand_read_phys_page_locked(q3n, entry->physical_block,
						entry->page, buf,
						Q3N_OP_FOREGROUND);
	if (ret)
		return ret;

	for (lane = 0; lane < Q3N_DATA_PAGES; lane++) {
		if (lane == missing_lane)
			continue;
		if (!q3n->data_page_valid[qemu_3dnand_data_page_index(q3n,
							       data_block,
							       stripe * Q3N_STRIPE_PAGES + lane)])
			return -EIO;
		ret = qemu_3dnand_read_phys_page_locked(q3n, data_block,
					 stripe * Q3N_STRIPE_PAGES + lane,
							q3n->page_buf,
							Q3N_OP_FOREGROUND);
		if (ret)
			return ret;
		for (i = 0; i < q3n->page_size; i++)
			buf[i] ^= q3n->page_buf[i];
	}

	q3n->raid_recovered++;
	return 0;
}

static int qemu_3dnand_read_data_page_locked(struct qemu_3dnand *q3n,
					     u32 data_block, u32 page, u8 *buf)
{
	int ret;

	ret = qemu_3dnand_read_phys_page_locked(q3n, data_block, page, buf,
						Q3N_OP_FOREGROUND);
	if (!ret)
		return 0;

	ret = qemu_3dnand_recover_page_locked(q3n, data_block, page, buf);
	if (ret)
		q3n->raid_failed++;
	return ret;
}

static int qemu_3dnand_mtd_read(struct mtd_info *mtd, loff_t from, size_t len,
				size_t *retlen, u_char *buf)
{
	struct qemu_3dnand *q3n = mtd->priv;
	struct q3n_request req = {
		.class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_READ,
	};
	size_t done = 0;
	int ret = 0;

	if (from < 0 || from + len > mtd->size)
		return -EINVAL;

	ret = qemu_3dnand_lock_request(q3n, &req);
	if (ret)
		return ret;
	while (done < len) {
		u32 block, page, column;
		size_t chunk;

		qemu_3dnand_decode_logical(q3n, from + done, &block, &page,
					   &column);
		if (q3n_block_is_cancelling(
				&q3n->data_meta[block].parity_barrier)) {
			ret = -EBUSY;
			break;
		}
		chunk = min_t(size_t, len - done, q3n->page_size - column);
		ret = qemu_3dnand_read_data_page_locked(q3n, block, page,
							q3n->page_buf);
		if (ret)
			break;
		memcpy(buf + done, q3n->page_buf + column, chunk);
		done += chunk;
	}
	mutex_unlock(&q3n->mtd_lock);

	*retlen = done;
	return ret;
}

static int qemu_3dnand_mtd_read_oob(struct mtd_info *mtd, loff_t from,
				     struct mtd_oob_ops *ops)
{
	struct qemu_3dnand *q3n = mtd->priv;
	struct q3n_request req = {
		.class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_READ,
	};
	size_t data_done = 0;
	size_t oob_done = 0;
	u32 ooboffs;
	u64 logical_page;
	u32 column;
	int ret = 0;

	if (!ops)
		return -EINVAL;
	ops->retlen = 0;
	ops->oobretlen = 0;
	if (ops->mode != MTD_OPS_PLACE_OOB && ops->mode != MTD_OPS_RAW)
		return -EOPNOTSUPP;
	if (from < 0 || from + ops->len > mtd->size)
		return -EINVAL;
	if (ops->ooboffs >= Q3N_LOGICAL_OOB_SIZE && ops->ooblen)
		return -EINVAL;
	if (!ops->oobbuf)
		return qemu_3dnand_mtd_read(mtd, from, ops->len,
					    &ops->retlen, ops->datbuf);

	logical_page = div64_u64(from, q3n->page_size);
	column = from % q3n->page_size;
	ooboffs = ops->ooboffs;
	ret = qemu_3dnand_lock_request(q3n, &req);
	if (ret)
		return ret;
	while (data_done < ops->len || oob_done < ops->ooblen) {
		struct q3n_ecc_result ecc;
		loff_t page_addr = logical_page * q3n->page_size;
		size_t data_chunk = min_t(size_t, ops->len - data_done,
					       q3n->page_size - column);
		size_t oob_chunk = min_t(size_t, ops->ooblen - oob_done,
					      Q3N_LOGICAL_OOB_SIZE - ooboffs);
		u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
		u32 block, page, ignored;

		qemu_3dnand_decode_logical(q3n, page_addr, &block, &page,
					   &ignored);
		if (q3n_block_is_cancelling(
				&q3n->data_meta[block].parity_barrier)) {
			ret = -EBUSY;
			break;
		}
		ret = qemu_3dnand_read_phys_page_oob_locked(q3n, block, page,
				q3n->page_buf, logical_oob, Q3N_OP_FOREGROUND,
				&ecc);
		if (ret)
			break;
		if (data_chunk)
			memcpy(ops->datbuf + data_done,
			       q3n->page_buf + column, data_chunk);
		if (oob_chunk)
			memcpy(ops->oobbuf + oob_done, logical_oob + ooboffs,
			       oob_chunk);
		data_done += data_chunk;
		oob_done += oob_chunk;
		logical_page++;
		column = 0;
		ooboffs = 0;
	}
	mutex_unlock(&q3n->mtd_lock);

	ops->retlen = data_done;
	ops->oobretlen = oob_done;
	return ret;
}

static int qemu_3dnand_program_logical_page(struct qemu_3dnand *q3n,
					    u32 block, u32 page,
					    const u8 *data,
					    const u8 *oob, u32 ooboffs,
					    size_t ooblen)
{
	struct q3n_request req = {
		.class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_PROGRAM,
		.block_state = &q3n->program_state[block],
		.page = page,
	};
	struct q3n_data_meta page_meta;
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	const u8 *program_data = data;
	u32 stripe;
	bool parity_reserved = false;
	int ret;

	mutex_lock(&q3n->mtd_lock);
	ret = q3n->data_meta[block].bad ? -EIO : 0;
	mutex_unlock(&q3n->mtd_lock);
	if (ret)
		return ret;
	if (page % Q3N_STRIPE_PAGES == Q3N_DATA_PAGES - 1) {
		ret = qemu_3dnand_reserve_parity(q3n);
		if (ret)
			return ret;
		parity_reserved = true;
	}
	ret = qemu_3dnand_lock_request(q3n, &req);
	if (ret)
		goto out_release_parity;
	if (q3n_block_is_cancelling(
			&q3n->data_meta[block].parity_barrier)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	if (q3n->data_meta[block].bad) {
		ret = -EIO;
		goto out_unlock;
	}

	if (data) {
		memset(&page_meta, 0, sizeof(page_meta));
		page_meta.slot = page % Q3N_STRIPE_PAGES;
		page_meta.stripe_id = cpu_to_le64(qemu_3dnand_stripe_id(
			q3n, block, page / Q3N_STRIPE_PAGES));
		page_meta.data_crc = cpu_to_le32(crc32_le(~0, data,
							       q3n->page_size));
		ret = q3n_pack_data_oob(logical_oob, sizeof(logical_oob),
					     &page_meta);
	} else {
		memset(q3n->page_buf, 0xff, q3n->page_size);
		memset(logical_oob, 0xff, sizeof(logical_oob));
		program_data = q3n->page_buf;
		ret = 0;
	}
	if (!ret && ooblen)
		memcpy(logical_oob + ooboffs, oob, ooblen);
	if (!ret)
		ret = qemu_3dnand_program_phys_page_oob_locked(q3n, block,
			page, program_data, logical_oob, Q3N_OP_FOREGROUND);
	if (ret)
		goto out_unlock;

	q3n->data_page_valid[qemu_3dnand_data_page_index(q3n, block,
							      page)] = 1;
	q3n->data_meta[block].erased = false;
	if (page == 0 && logical_oob[0] != 0xff)
		q3n->data_meta[block].bad = true;
	stripe = page / Q3N_STRIPE_PAGES;
	if (parity_reserved) {
		int parity_ret;

		atomic64_inc(&q3n->unprotected_stripes);
		parity_ret = qemu_3dnand_queue_parity_locked(q3n, block,
							stripe);
		if (parity_ret)
			atomic64_inc(&q3n->failed_stripes);
	}
	mutex_unlock(&q3n->mtd_lock);
	return 0;

out_unlock:
	mutex_unlock(&q3n->mtd_lock);
out_release_parity:
	if (parity_reserved)
		q3n_sched_release_parity(&q3n->sched);
	return ret;
}

static int qemu_3dnand_mtd_write(struct mtd_info *mtd, loff_t to, size_t len,
				 size_t *retlen, const u_char *buf)
{
	struct qemu_3dnand *q3n = mtd->priv;
	size_t done = 0;
	int ret = 0;

	if (to < 0 || to + len > mtd->size)
		return -EINVAL;
	if (!IS_ALIGNED(to, q3n->page_size) || !IS_ALIGNED(len, q3n->page_size))
		return -EINVAL;

	while (done < len) {
		u32 block, page, column;

		qemu_3dnand_decode_logical(q3n, to + done, &block, &page,
					   &column);
		ret = qemu_3dnand_program_logical_page(q3n, block, page,
							buf + done, NULL, 0, 0);
		if (ret)
			break;
		done += q3n->page_size;
	}

	*retlen = done;
	return ret;
}

static int qemu_3dnand_mtd_write_oob(struct mtd_info *mtd, loff_t to,
				      struct mtd_oob_ops *ops)
{
	struct qemu_3dnand *q3n = mtd->priv;
	size_t data_done = 0;
	size_t oob_done = 0;
	u64 logical_page;
	u32 ooboffs;
	int ret = 0;

	if (!ops)
		return -EINVAL;
	ops->retlen = 0;
	ops->oobretlen = 0;
	if (ops->mode != MTD_OPS_PLACE_OOB && ops->mode != MTD_OPS_RAW)
		return -EOPNOTSUPP;
	if (to < 0 || to + ops->len > mtd->size)
		return -EINVAL;
	if (ops->ooboffs >= Q3N_LOGICAL_OOB_SIZE && ops->ooblen)
		return -EINVAL;
	if (!ops->oobbuf)
		return qemu_3dnand_mtd_write(mtd, to, ops->len,
					     &ops->retlen, ops->datbuf);
	if (ops->datbuf &&
	    (!IS_ALIGNED(to, q3n->page_size) ||
	     !IS_ALIGNED(ops->len, q3n->page_size)))
		return -EINVAL;

	logical_page = div64_u64(to, q3n->page_size);
	ooboffs = ops->ooboffs;
	while (data_done < ops->len || oob_done < ops->ooblen) {
		loff_t page_addr = logical_page * q3n->page_size;
		size_t oob_chunk = min_t(size_t, ops->ooblen - oob_done,
					      Q3N_LOGICAL_OOB_SIZE - ooboffs);
		const u8 *data = data_done < ops->len ?
			ops->datbuf + data_done : NULL;
		u32 block, page, column;

		qemu_3dnand_decode_logical(q3n, page_addr, &block, &page,
					   &column);
		ret = qemu_3dnand_program_logical_page(q3n, block, page, data,
				ops->oobbuf + oob_done, ooboffs, oob_chunk);
		if (ret) {
			if (ret == -ESTALE)
				ret = -EIO;
			break;
		}
		if (data)
			data_done += q3n->page_size;
		oob_done += oob_chunk;
		logical_page++;
		ooboffs = 0;
	}

	ops->retlen = data_done;
	ops->oobretlen = oob_done;
	return ret;
}

static void qemu_3dnand_invalidate_block_parity(struct qemu_3dnand *q3n,
						u32 block)
{
	u32 page;

	if (block >= q3n->data_block_count)
		return;

	for (page = 0; page < q3n->pages_per_block / Q3N_STRIPE_PAGES; page++) {
		struct qemu_3dnand_parity_entry *entry;

		entry = &q3n->parity_index[qemu_3dnand_parity_index(q3n, block,
							     page)];
		if (!qemu_3dnand_parity_generation_valid(q3n, entry, block))
			qemu_3dnand_mark_parity_stale(q3n, entry);
	}
}

static bool qemu_3dnand_pending_parity_drained(
		const struct q3n_block_barrier *barrier)
{
	return q3n_block_pending(barrier) == 0;
}

static int qemu_3dnand_cancel_block_parity(struct qemu_3dnand *q3n,
					    u32 block)
{
	struct q3n_block_barrier *barrier =
		&q3n->data_meta[block].parity_barrier;
	int ret;

	ret = q3n_block_cancel_begin(barrier);
	if (ret)
		return ret;
	q3n_sched_notify(&q3n->sched);
	wake_up_all(&q3n->parity_pause_waitq);
	mutex_unlock(&q3n->mtd_lock);
	wait_event(q3n->parity_cancel_waitq, qemu_3dnand_pending_parity_drained(barrier));
	mutex_lock(&q3n->mtd_lock);
	return 0;
}

static int qemu_3dnand_mtd_erase(struct mtd_info *mtd,
				 struct erase_info *instr)
{
	struct qemu_3dnand *q3n = mtd->priv;
	u64 done = 0;
	int ret = 0;

	if (instr->addr + instr->len > mtd->size)
		return -EINVAL;
	if (instr->addr % mtd->erasesize || instr->len % mtd->erasesize)
		return -EINVAL;

	mutex_lock(&q3n->mtd_lock);
	while (done < instr->len) {
		u32 block, page, column;

		qemu_3dnand_decode_logical(q3n, instr->addr + done, &block,
					   &page, &column);
		if (q3n->data_meta[block].bad) {
			instr->fail_addr = instr->addr + done;
			ret = -EIO;
			break;
		}
		ret = qemu_3dnand_cancel_block_parity(q3n, block);
		if (ret) {
			instr->fail_addr = instr->addr + done;
			break;
		}
		ret = qemu_3dnand_erase_phys_block_locked(q3n, block);
		if (ret) {
			instr->fail_addr = instr->addr + done;
			q3n_block_cancel_end(
				&q3n->data_meta[block].parity_barrier);
			break;
		}
		memset(&q3n->data_page_valid[block * q3n->pages_per_block], 0,
		       q3n->pages_per_block);
		q3n->data_meta[block].generation++;
		if (!q3n->data_meta[block].generation)
			q3n->data_meta[block].generation = 1;
		q3n->data_meta[block].erased = true;
		q3n->program_state[block].next_prog_page = 0;
		q3n->generation_updates++;
		qemu_3dnand_invalidate_block_parity(q3n, block);
		q3n_block_cancel_end(&q3n->data_meta[block].parity_barrier);
		done += mtd->erasesize;
	}
	mutex_unlock(&q3n->mtd_lock);

	return ret;
}

static void qemu_3dnand_mtd_sync(struct mtd_info *mtd)
{
	struct qemu_3dnand *q3n = mtd->priv;

	flush_workqueue(q3n->parity_wq);
	mutex_lock(&q3n->mtd_lock);
	q3n_sched_drain(&q3n->sched);
	mutex_unlock(&q3n->mtd_lock);
}

static int qemu_3dnand_mtd_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	struct qemu_3dnand *q3n = mtd->priv;
	u64 block;
	u32 status;
	u32 next_page;
	int ret;
	int bad;

	if (ofs < 0 || ofs >= mtd->size)
		return -EINVAL;
	block = div64_u64(ofs, mtd->erasesize);
	if (block >= q3n->data_block_count)
		return -EINVAL;

	mutex_lock(&q3n->mtd_lock);
	ret = qemu_3dnand_get_phys_block_status_locked(q3n, block, &status,
						       &next_page);
	if (ret) {
		bad = ret;
	} else {
		q3n->data_meta[block].bad = status & Q3N_BLOCK_STATUS_BAD;
		bad = q3n->data_meta[block].bad;
	}
	mutex_unlock(&q3n->mtd_lock);
	return bad;
}

static int qemu_3dnand_mtd_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	struct qemu_3dnand *q3n = mtd->priv;
	u64 block;
	int ret;

	if (ofs < 0 || ofs >= mtd->size)
		return -EINVAL;
	if (!(q3n->cap & Q3N_CAP_BAD_BLOCK_MARKER))
		return -EOPNOTSUPP;
	block = div64_u64(ofs, mtd->erasesize);
	if (block >= q3n->data_block_count)
		return -EINVAL;

	mutex_lock(&q3n->mtd_lock);
	if (q3n->data_meta[block].bad) {
		ret = 0;
		goto out_unlock;
	}
	ret = qemu_3dnand_cancel_block_parity(q3n, block);
	if (ret)
		goto out_unlock;

	ret = qemu_3dnand_mark_phys_block_bad_locked(q3n, block);
	if (!ret)
		q3n->data_meta[block].bad = true;
	q3n_block_cancel_end(&q3n->data_meta[block].parity_barrier);

out_unlock:
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static int qemu_3dnand_register_mtd(struct qemu_3dnand *q3n)
{
	struct mtd_info *mtd = &q3n->mtd;

	mtd->name = "qemu-3dnand";
	mtd->type = MTD_NANDFLASH;
	mtd->flags = MTD_CAP_NANDFLASH;
	mtd->size = (u64)q3n->data_block_count *
		(q3n->pages_per_block / Q3N_STRIPE_PAGES) *
		Q3N_DATA_PAGES * q3n->page_size;
	mtd->erasesize = (q3n->pages_per_block / Q3N_STRIPE_PAGES) *
		Q3N_DATA_PAGES * q3n->page_size;
	mtd->writesize = q3n->page_size;
	mtd->writebufsize = q3n->page_size;
	mtd->oobsize = Q3N_LOGICAL_OOB_SIZE;
	mtd->owner = THIS_MODULE;
	mtd->priv = q3n;
	mtd->_read_oob = qemu_3dnand_mtd_read_oob;
	mtd->_write_oob = qemu_3dnand_mtd_write_oob;
	mtd->_erase = qemu_3dnand_mtd_erase;
	mtd->_sync = qemu_3dnand_mtd_sync;
	mtd->_block_isbad = qemu_3dnand_mtd_block_isbad;
	mtd->_block_markbad = qemu_3dnand_mtd_block_markbad;
	mtd->dev.parent = &q3n->pdev->dev;

	return mtd_device_register(mtd, NULL, 0);
}

static int qemu_3dnand_inject_data_loss(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;
	int ret;

	mutex_lock(&q3n->mtd_lock);
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_LO, lower_32_bits(value));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_HI, upper_32_bits(value));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_CTRL,
			   Q3N_FAULT_INJECT_DATA_LOSS);
	ret = qemu_3dnand_wait_ready(q3n);
	mutex_unlock(&q3n->mtd_lock);

	return ret;
}

static int qemu_3dnand_inject_parity_program_fail(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;
	u32 block;
	u32 page;
	u32 column;
	u32 parity_page;
	loff_t phys_addr;
	int ret;

	if (value >= q3n->mtd.size)
		return -ERANGE;
	qemu_3dnand_decode_logical(q3n, value, &block, &page, &column);
	if (column)
		return -EINVAL;
	parity_page = (page / Q3N_STRIPE_PAGES) * Q3N_STRIPE_PAGES +
		Q3N_DATA_PAGES;
	phys_addr = qemu_3dnand_phys_addr(q3n, block, parity_page);

	mutex_lock(&q3n->mtd_lock);
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_LO,
			   lower_32_bits(phys_addr));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_HI,
			   upper_32_bits(phys_addr));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_CTRL,
			   Q3N_FAULT_FAIL_NEXT_PROGRAM);
	ret = qemu_3dnand_wait_ready(q3n);
	mutex_unlock(&q3n->mtd_lock);

	return ret;
}

static int qemu_3dnand_cancel_parity_program_fail(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;
	int ret;

	if (value != 1)
		return -EINVAL;
	mutex_lock(&q3n->mtd_lock);
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_CTRL, 0);
	ret = qemu_3dnand_wait_ready(q3n);
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static int qemu_3dnand_reset_controller(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;
	int ret;

	if (value != 1)
		return -EINVAL;
	mutex_lock(&q3n->mtd_lock);
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_RESET);
	ret = qemu_3dnand_wait_ready(q3n);
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static int qemu_3dnand_inject_invalid_program_fail(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;
	loff_t invalid_addr = ~(loff_t)(q3n->page_size - 1);
	int ret;

	if (value != 1)
		return -EINVAL;
	mutex_lock(&q3n->mtd_lock);
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_LO,
			   lower_32_bits(invalid_addr));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_HI,
			   upper_32_bits(invalid_addr));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_CTRL,
			   Q3N_FAULT_FAIL_NEXT_PROGRAM);
	ret = qemu_3dnand_wait_ready(q3n);
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static int qemu_3dnand_inject_parity_queue_fail(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;

	if (value != 1)
		return -EINVAL;
	atomic_set(&q3n->fail_next_parity_queue, 1);
	return 0;
}

#define Q3N_MMIO_STAT_GETTER(_name, _reg) \
static int qemu_3dnand_##_name##_get(void *data, u64 *value) \
{ \
	struct qemu_3dnand *q3n = data; \
	*value = qemu_3dnand_readl(q3n, _reg); \
	return 0; \
}

Q3N_MMIO_STAT_GETTER(foreground_ops, Q3N_REG_STAT_FG_OPS)
Q3N_MMIO_STAT_GETTER(parity_reads, Q3N_REG_STAT_PARITY_READS)
Q3N_MMIO_STAT_GETTER(parity_writes, Q3N_REG_STAT_PARITY_WRITES)
Q3N_MMIO_STAT_GETTER(order_errors, Q3N_REG_STAT_ORDER_ERRORS)

static int qemu_3dnand_raid_recovered_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->raid_recovered;
	return 0;
}

static int qemu_3dnand_raid_failed_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->raid_failed;
	return 0;
}

static int qemu_3dnand_parity_stale_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->parity_stale;
	return 0;
}

static int qemu_3dnand_faults_injected_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = qemu_3dnand_readl(q3n, Q3N_REG_STAT_FAULTS_INJECTED);
	return 0;
}

static int qemu_3dnand_parity_written_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->parity_written;
	return 0;
}

static int qemu_3dnand_generation_updates_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->generation_updates;
	return 0;
}

static int qemu_3dnand_protected_stripes_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = atomic64_read(&q3n->protected_stripes);
	return 0;
}

static int qemu_3dnand_unprotected_stripes_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = atomic64_read(&q3n->unprotected_stripes);
	return 0;
}

static int qemu_3dnand_failed_stripes_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = atomic64_read(&q3n->failed_stripes);
	return 0;
}

static int qemu_3dnand_parity_pause_block_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = READ_ONCE(q3n->parity_pause_block);
	return 0;
}

static int qemu_3dnand_parity_pause_block_set(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;

	if (value >= q3n->data_block_count)
		return -ERANGE;
	WRITE_ONCE(q3n->parity_pause_block, value);
	wake_up_all(&q3n->parity_pause_waitq);
	return 0;
}

static int qemu_3dnand_parity_pause_enable_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = READ_ONCE(q3n->parity_pause_enable);
	return 0;
}

static int qemu_3dnand_parity_pause_enable_set(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;

	if (value > 1)
		return -EINVAL;
	WRITE_ONCE(q3n->parity_pause_enable, value);
	if (!value)
		wake_up_all(&q3n->parity_pause_waitq);
	return 0;
}

static int qemu_3dnand_parity_paused_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = atomic_read(&q3n->parity_paused);
	return 0;
}

static int qemu_3dnand_parity_continuation_pause_block_get(void *data,
							    u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = READ_ONCE(q3n->parity_continuation_pause_block);
	return 0;
}

static int qemu_3dnand_parity_continuation_pause_block_set(void *data,
							    u64 value)
{
	struct qemu_3dnand *q3n = data;

	if (value >= q3n->data_block_count)
		return -ERANGE;
	WRITE_ONCE(q3n->parity_continuation_pause_block, value);
	wake_up_all(&q3n->parity_pause_waitq);
	return 0;
}

static int qemu_3dnand_parity_continuation_pause_enable_get(void *data,
							     u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = READ_ONCE(q3n->parity_continuation_pause_enable);
	return 0;
}

static int qemu_3dnand_parity_continuation_pause_class_get(void *data,
							    u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = READ_ONCE(q3n->parity_continuation_pause_class);
	return 0;
}

static int qemu_3dnand_parity_continuation_pause_class_set(void *data,
							    u64 value)
{
	struct qemu_3dnand *q3n = data;

	if (value != Q3N_REQ_PARITY_READ && value != Q3N_REQ_PARITY_WRITE)
		return -EINVAL;
	WRITE_ONCE(q3n->parity_continuation_pause_class, value);
	wake_up_all(&q3n->parity_pause_waitq);
	return 0;
}

static int qemu_3dnand_parity_continuation_pause_enable_set(void *data,
							     u64 value)
{
	struct qemu_3dnand *q3n = data;

	if (value > 1)
		return -EINVAL;
	WRITE_ONCE(q3n->parity_continuation_pause_enable, value);
	if (!value)
		wake_up_all(&q3n->parity_pause_waitq);
	return 0;
}

static int qemu_3dnand_parity_continuation_paused_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = atomic_read(&q3n->parity_continuation_paused);
	return 0;
}

static int qemu_3dnand_pending_parity_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;
	u32 pending;
	u32 reserved;

	q3n_sched_get_counts(&q3n->sched, &pending, &reserved, NULL);
	*value = pending;
	return 0;
}

static int qemu_3dnand_reserved_parity_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;
	u32 pending;
	u32 reserved;

	q3n_sched_get_counts(&q3n->sched, &pending, &reserved, NULL);
	*value = reserved;
	return 0;
}

static int qemu_3dnand_max_pending_parity_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;
	u32 pending;
	u32 reserved;
	u32 max_pending;

	q3n_sched_get_counts(&q3n->sched, &pending, &reserved, &max_pending);
	*value = max_pending;
	return 0;
}

static int qemu_3dnand_p1_over_p2_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n_sched_get_p1_over_p2(&q3n->sched);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_inject_data_loss_fops, NULL,
			 qemu_3dnand_inject_data_loss, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_inject_parity_program_fail_fops, NULL,
			 qemu_3dnand_inject_parity_program_fail, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_cancel_parity_program_fail_fops, NULL,
			 qemu_3dnand_cancel_parity_program_fail, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_reset_controller_fops, NULL,
			 qemu_3dnand_reset_controller, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_inject_invalid_program_fail_fops, NULL,
			 qemu_3dnand_inject_invalid_program_fail, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_inject_parity_queue_fail_fops, NULL,
			 qemu_3dnand_inject_parity_queue_fail, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_foreground_ops_fops,
			 qemu_3dnand_foreground_ops_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_reads_fops,
			 qemu_3dnand_parity_reads_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_writes_fops,
			 qemu_3dnand_parity_writes_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_order_errors_fops,
			 qemu_3dnand_order_errors_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_raid_recovered_fops,
			 qemu_3dnand_raid_recovered_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_raid_failed_fops,
			 qemu_3dnand_raid_failed_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_stale_fops,
			 qemu_3dnand_parity_stale_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_faults_injected_fops,
			 qemu_3dnand_faults_injected_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_written_fops,
			 qemu_3dnand_parity_written_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_generation_updates_fops,
			 qemu_3dnand_generation_updates_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_protected_stripes_fops,
			 qemu_3dnand_protected_stripes_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_unprotected_stripes_fops,
			 qemu_3dnand_unprotected_stripes_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_failed_stripes_fops,
			 qemu_3dnand_failed_stripes_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_pause_block_fops,
			 qemu_3dnand_parity_pause_block_get,
			 qemu_3dnand_parity_pause_block_set, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_pause_enable_fops,
			 qemu_3dnand_parity_pause_enable_get,
			 qemu_3dnand_parity_pause_enable_set, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_paused_fops,
			 qemu_3dnand_parity_paused_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_continuation_pause_block_fops,
			 qemu_3dnand_parity_continuation_pause_block_get,
			 qemu_3dnand_parity_continuation_pause_block_set, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_continuation_pause_enable_fops,
			 qemu_3dnand_parity_continuation_pause_enable_get,
			 qemu_3dnand_parity_continuation_pause_enable_set, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_continuation_pause_class_fops,
			 qemu_3dnand_parity_continuation_pause_class_get,
			 qemu_3dnand_parity_continuation_pause_class_set, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_parity_continuation_paused_fops,
			 qemu_3dnand_parity_continuation_paused_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_pending_parity_fops,
			 qemu_3dnand_pending_parity_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_reserved_parity_fops,
			 qemu_3dnand_reserved_parity_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_max_pending_parity_fops,
			 qemu_3dnand_max_pending_parity_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_p1_over_p2_fops,
			 qemu_3dnand_p1_over_p2_get, NULL, "%llu\n");

static void qemu_3dnand_debugfs_init(struct qemu_3dnand *q3n)
{
	q3n->debugfs_dir = debugfs_create_dir("qemu_3dnand", NULL);
	debugfs_create_file("inject_data_loss", 0200, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_inject_data_loss_fops);
	debugfs_create_file("inject_parity_program_fail", 0200,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_inject_parity_program_fail_fops);
	debugfs_create_file("cancel_parity_program_fail", 0200,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_cancel_parity_program_fail_fops);
	debugfs_create_file("reset_controller", 0200, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_reset_controller_fops);
	debugfs_create_file("inject_invalid_program_fail", 0200,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_inject_invalid_program_fail_fops);
	debugfs_create_file("inject_parity_queue_fail", 0200,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_inject_parity_queue_fail_fops);
	debugfs_create_file("foreground_ops", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_foreground_ops_fops);
	debugfs_create_file("parity_reads", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_reads_fops);
	debugfs_create_file("parity_writes", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_writes_fops);
	debugfs_create_file("order_errors", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_order_errors_fops);
	debugfs_create_file("raid_recovered", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_raid_recovered_fops);
	debugfs_create_file("raid_failed", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_raid_failed_fops);
	debugfs_create_file("parity_stale", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_stale_fops);
	debugfs_create_file("parity_written", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_written_fops);
	debugfs_create_file("generation_updates", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_generation_updates_fops);
	debugfs_create_file("protected_stripes", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_protected_stripes_fops);
	debugfs_create_file("unprotected_stripes", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_unprotected_stripes_fops);
	debugfs_create_file("failed_stripes", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_failed_stripes_fops);
	debugfs_create_file("faults_injected", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_faults_injected_fops);
	debugfs_create_file("parity_pause_block", 0600, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_pause_block_fops);
	debugfs_create_file("parity_pause_enable", 0600, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_pause_enable_fops);
	debugfs_create_file("parity_paused", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_paused_fops);
	debugfs_create_file("parity_continuation_pause_block", 0600,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_continuation_pause_block_fops);
	debugfs_create_file("parity_continuation_pause_enable", 0600,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_continuation_pause_enable_fops);
	debugfs_create_file("parity_continuation_pause_class", 0600,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_continuation_pause_class_fops);
	debugfs_create_file("parity_continuation_paused", 0400,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_parity_continuation_paused_fops);
	debugfs_create_file("pending_parity", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_pending_parity_fops);
	debugfs_create_file("reserved_parity", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_reserved_parity_fops);
	debugfs_create_file("max_pending_parity", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_max_pending_parity_fops);
	debugfs_create_file("p1_over_p2", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_p1_over_p2_fops);
}

static int qemu_3dnand_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct qemu_3dnand *q3n;
	struct resource bar;
	u32 ident;
	u32 cap;
	u32 geom0;
	u32 geom1;
	u32 pool0;
	u32 pool1;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM))
		return dev_err_probe(dev, -ENODEV, "BAR0 is not MMIO\n");

	q3n = devm_kzalloc(dev, sizeof(*q3n), GFP_KERNEL);
	if (!q3n)
		return -ENOMEM;

	memset(&bar, 0, sizeof(bar));
	bar.name = "qemu_3dnand_mmio";
	bar.start = pci_resource_start(pdev, 0);
	bar.end = pci_resource_end(pdev, 0);
	bar.flags = IORESOURCE_MEM;

	q3n->pdev = pdev;
	q3n->regs_size = resource_size(&bar);
	q3n->regs = devm_ioremap_resource(dev, &bar);
	if (IS_ERR(q3n->regs))
		return PTR_ERR(q3n->regs);

	mutex_init(&q3n->mtd_lock);
	q3n_sched_init(&q3n->sched);
	init_waitqueue_head(&q3n->parity_cancel_waitq);
	init_waitqueue_head(&q3n->parity_pause_waitq);
	atomic_set(&q3n->parity_paused, 0);
	atomic_set(&q3n->parity_continuation_paused, 0);
	atomic_set(&q3n->fail_next_parity_queue, 0);
	atomic64_set(&q3n->protected_stripes, 0);
	atomic64_set(&q3n->unprotected_stripes, 0);
	atomic64_set(&q3n->failed_stripes, 0);
	q3n->parity_continuation_pause_class = Q3N_REQ_PARITY_READ;
	q3n->parity_wq = alloc_workqueue("q3n-parity", WQ_UNBOUND,
					 Q3N_MAX_PENDING_PARITY);
	if (!q3n->parity_wq)
		return -ENOMEM;
	pci_set_drvdata(pdev, q3n);

	ident = qemu_3dnand_readl(q3n, Q3N_REG_ID);
	if (ident != Q3N_ID_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected q3n id 0x%08x\n", ident);

	cap = qemu_3dnand_readl(q3n, Q3N_REG_CAP);
	q3n->cap = cap;
	geom0 = qemu_3dnand_readl(q3n, Q3N_REG_GEOM0);
	geom1 = qemu_3dnand_readl(q3n, Q3N_REG_GEOM1);
	pool0 = qemu_3dnand_readl(q3n, Q3N_REG_POOL0);
	pool1 = qemu_3dnand_readl(q3n, Q3N_REG_POOL1);

	q3n->page_size = geom0 & 0xffff;
	q3n->oob_size = geom0 >> 16;
	q3n->pages_per_block = geom1 & 0xffff;
	q3n->blocks_per_plane = geom1 >> 16;
	q3n->data_blocks_per_plane = pool0 & 0xffff;
	q3n->parity_blocks_per_plane = pool0 >> 16;
	q3n->metadata_blocks_per_plane = pool1 & 0xffff;
	q3n->reserve_blocks_per_plane = pool1 >> 16;
	q3n->data_block_count = q3n->data_blocks_per_plane * Q3N_RAID_LANES;
	q3n->parity_block_count = q3n->parity_blocks_per_plane * Q3N_RAID_LANES;
	q3n->raid_group_count = q3n->data_block_count / Q3N_RAID_LANES;

	q3n->page_buf = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
	q3n->raid_buf = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
	/* These arrays are multi-megabyte with the 2-die x 4-plane geometry. */
	q3n->data_page_valid = kvcalloc(q3n->data_block_count,
					  q3n->pages_per_block,
					  GFP_KERNEL);
	q3n->data_meta = devm_kcalloc(dev, q3n->data_block_count,
				      sizeof(*q3n->data_meta), GFP_KERNEL);
	q3n->program_state = devm_kcalloc(dev, q3n->data_block_count,
					  sizeof(*q3n->program_state), GFP_KERNEL);
	q3n->parity_index = kvcalloc(q3n->data_block_count *
					     (q3n->pages_per_block /
					      Q3N_STRIPE_PAGES),
					     sizeof(*q3n->parity_index),
					     GFP_KERNEL);
	if (!q3n->page_buf || !q3n->raid_buf || !q3n->data_page_valid ||
	    !q3n->program_state ||
	    !q3n->data_meta || !q3n->parity_index)
		goto err_free_metadata;

	for (ret = 0; ret < q3n->data_block_count; ret++) {
		q3n->data_meta[ret].generation = 1;
		q3n->data_meta[ret].erased = true;
		q3n_block_barrier_init(&q3n->data_meta[ret].parity_barrier);
	}
	if (q3n->cap & Q3N_CAP_PERSISTENT_MEDIA) {
		mutex_lock(&q3n->mtd_lock);
		ret = qemu_3dnand_restore_media_locked(q3n);
		mutex_unlock(&q3n->mtd_lock);
		if (ret)
			goto err_free_metadata;
	}

	ret = qemu_3dnand_register_mtd(q3n);
	if (ret)
		goto err_free_metadata;
	qemu_3dnand_debugfs_init(q3n);

	dev_info(dev,
		 "q3n NAND: page=%u oob=%u pages/block=%u blocks/plane=%u cap=0x%x\n",
		 q3n->page_size, q3n->oob_size, q3n->pages_per_block,
		 q3n->blocks_per_plane, cap);
	dev_info(dev,
		 "q3n driver RAID: data=%u parity=%u metadata=%u reserve=%u bar=%pa size=%pa mtd=%s\n",
		 q3n->data_blocks_per_plane, q3n->parity_blocks_per_plane,
		 q3n->metadata_blocks_per_plane, q3n->reserve_blocks_per_plane,
		 &bar.start, &q3n->regs_size, q3n->mtd.name);

	return 0;

err_free_metadata:
	if (q3n->parity_wq)
		destroy_workqueue(q3n->parity_wq);
	qemu_3dnand_free_metadata(q3n);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register MTD\n");
	return -ENOMEM;
}

static void qemu_3dnand_remove(struct pci_dev *pdev)
{
	struct qemu_3dnand *q3n = pci_get_drvdata(pdev);

	if (q3n) {
		debugfs_remove_recursive(q3n->debugfs_dir);
		WRITE_ONCE(q3n->parity_pause_enable, false);
		WRITE_ONCE(q3n->parity_continuation_pause_enable, false);
		wake_up_all(&q3n->parity_pause_waitq);
		mtd_device_unregister(&q3n->mtd);
		flush_workqueue(q3n->parity_wq);
		destroy_workqueue(q3n->parity_wq);
		qemu_3dnand_free_metadata(q3n);
	}
	pci_set_drvdata(pdev, NULL);
}

static const struct pci_device_id qemu_3dnand_id_table[] = {
	{ PCI_DEVICE(Q3N_PCI_VENDOR_ID, Q3N_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, qemu_3dnand_id_table);

static struct pci_driver qemu_3dnand_driver = {
	.name = "qemu_3dnand",
	.id_table = qemu_3dnand_id_table,
	.probe = qemu_3dnand_probe,
	.remove = qemu_3dnand_remove,
};
module_pci_driver(qemu_3dnand_driver);

MODULE_DESCRIPTION("QEMU 3D NAND PCI MTD driver with driver-owned page RAID");
MODULE_AUTHOR("OpenAI Codex");
MODULE_LICENSE("GPL");
