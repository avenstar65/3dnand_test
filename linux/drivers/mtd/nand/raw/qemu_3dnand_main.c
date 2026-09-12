// SPDX-License-Identifier: GPL-2.0
/*
 * Linux MTD driver for the QEMU 3D NAND controller model.
 *
 * QEMU provides only physical flash/controller semantics. This driver owns the
 * same-block serial page-raid layout: seven data pages followed by one hidden
 * parity page (D0..D6,P) per stripe.
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
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

static unsigned int raid_level = Q3N_RAID5;
module_param(raid_level, uint, 0444);
MODULE_PARM_DESC(raid_level, "Page RAID level: 1 or 5");

struct qemu_3dnand_data_block_meta {
	u32 generation;
	bool bad;
	struct q3n_block_barrier parity_barrier;
};

struct qemu_3dnand_parity_entry {
	u32 physical_block;
	u32 page;
	u32 data_block_generation[Q3N_RAID_LANES];
	u32 parity_version;
	u64 sequence;
	bool valid;
};

struct qemu_3dnand {
	struct pci_dev *pdev;
	void __iomem *regs;
	struct q3n_mp_io mp;
	struct q3n_geometry profile_geometry;
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
	struct nand_controller controller;
	struct nand_chip chip;
	struct mtd_info *mtd;
	struct nand_flash_dev scan_ids[2];
	bool scanned;
	struct {
		u8 data[8];
		u8 len;
		u8 pos;
		u32 erase_page;
		int error;
		bool erase_pending;
	} legacy;
	bool core_markbad;

	u8 *page_buf;
	u8 *raid_buf;
	u8 *mp_buf[Q3N_PLANES_PER_DIE];
	u8 *mp_oob[Q3N_PLANES_PER_DIE];
	enum q3n_stripe_state *profile_state;
	u32 profile_leb_count;
	u8 *data_page_valid;
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
	u64 raid_source_corrected_bits;
	u64 metadata_degraded;
	u8 last_parity_plane;
	u64 background_ecc_corrected_bits;
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
	bool failure_accounted;
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
	kvfree(q3n->profile_state);
	q3n->profile_state = NULL;
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

static void qemu_3dnand_read_ecc_result(struct qemu_3dnand *q3n,
					struct q3n_ecc_result *ecc)
{
	u32 status;

	if (!ecc)
		return;

	status = qemu_3dnand_readl(q3n, Q3N_REG_ECC_STATUS);
	ecc->max_bitflips =
		qemu_3dnand_readl(q3n, Q3N_REG_ECC_MAX_BITFLIPS);
	ecc->corrected_bits =
		qemu_3dnand_readl(q3n, Q3N_REG_ECC_CORRECTED_BITS);
	ecc->failed_step = qemu_3dnand_readl(q3n, Q3N_REG_ECC_FAILED_STEP);
	ecc->uncorrectable = status & Q3N_ECC_STATUS_UNCORRECTABLE;
}

static void
qemu_3dnand_account_background_ecc(struct qemu_3dnand *q3n,
				   const struct q3n_ecc_result *ecc)
{
	q3n->background_ecc_corrected_bits += ecc->corrected_bits;
}

static void
qemu_3dnand_account_foreground_ecc(struct qemu_3dnand *q3n,
				   struct mtd_req_stats *stats,
				   const struct q3n_ecc_result *ecc)
{
	q3n->mtd->ecc_stats.corrected += ecc->corrected_bits;
	if (stats)
		stats->corrected_bitflips += ecc->corrected_bits;
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
					     u32 op_class,
					     struct q3n_ecc_result *ecc)
{
	u32 *words = (u32 *)buf;
	int ret;
	u32 i;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, page));
	qemu_3dnand_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	qemu_3dnand_writel(q3n, Q3N_REG_LEN, q3n->page_size);
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE);
	if (ecc)
		*ecc = (struct q3n_ecc_result) {};
	ret = qemu_3dnand_wait_ready(q3n);
	if (ret)
		return ret;

	for (i = 0; i < q3n->page_size / sizeof(u32); i++)
		words[i] = qemu_3dnand_readl(q3n, Q3N_REG_DATA);
	qemu_3dnand_read_ecc_result(q3n, ecc);

	return 0;
}

static int qemu_3dnand_read_phys_oob_locked(
		struct qemu_3dnand *q3n, u32 block, u32 page,
		u8 logical_oob[Q3N_LOGICAL_OOB_SIZE], u32 op_class)
{
	int ret;
	u32 i;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, page));
	qemu_3dnand_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	qemu_3dnand_writel(q3n, Q3N_REG_OOB_LEN, Q3N_LOGICAL_OOB_SIZE);
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_READ_PAGE_OOB);
	ret = qemu_3dnand_wait_ready(q3n);
	if (ret)
		return ret;

	for (i = 0; i < Q3N_LOGICAL_OOB_SIZE; i += sizeof(u32))
		put_unaligned_le32(qemu_3dnand_readl(q3n, Q3N_REG_DATA),
				   logical_oob + i);

	return 0;
}

static int qemu_3dnand_program_phys_page_locked(
		struct qemu_3dnand *q3n, u32 block, u32 page, const u8 *data,
		u32 op_class)
{
	int ret;
	u32 i;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, page));
	qemu_3dnand_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	qemu_3dnand_writel(q3n, Q3N_REG_LEN, Q3N_PAGE_SIZE);
	for (i = 0; i < Q3N_PAGE_SIZE; i += sizeof(u32))
		qemu_3dnand_writel(q3n, Q3N_REG_DATA,
				     get_unaligned_le32(data + i));
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_PROGRAM_PAGE);
	ret = qemu_3dnand_wait_ready(q3n);
	return ret;
}

static int qemu_3dnand_program_phys_oob_locked(
		struct qemu_3dnand *q3n, u32 block, u32 page,
		const u8 logical_oob[Q3N_LOGICAL_OOB_SIZE], u32 op_class)
{
	int ret;
	u32 i;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, page));
	qemu_3dnand_writel(q3n, Q3N_REG_OP_CLASS, op_class);
	qemu_3dnand_writel(q3n, Q3N_REG_OOB_LEN, Q3N_LOGICAL_OOB_SIZE);
	for (i = 0; i < Q3N_LOGICAL_OOB_SIZE; i += sizeof(u32))
		qemu_3dnand_writel(q3n, Q3N_REG_DATA,
				     get_unaligned_le32(logical_oob + i));

	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_PROGRAM_PAGE_OOB);
	ret = qemu_3dnand_wait_ready(q3n);
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

static void qemu_3dnand_build_bad_block_oob(u8 *logical_oob)
{
	memset(logical_oob, 0xff, Q3N_LOGICAL_OOB_SIZE);
	logical_oob[0] = 0x00;
}

static int qemu_3dnand_get_phys_block_status_locked(
		struct qemu_3dnand *q3n, u32 block, u32 *status)
{
	int ret;

	if (block >= q3n->data_block_count || !status)
		return -EINVAL;

	qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, 0));
	qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_GET_BLOCK_STATUS);
	ret = qemu_3dnand_wait_ready(q3n);
	if (ret)
		return ret;

	*status = qemu_3dnand_readl(q3n, Q3N_REG_BLOCK_STATUS);
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

static void qemu_3dnand_account_unprotected(
		struct qemu_3dnand_parity_work *parity)
{
	if (parity->failure_accounted)
		return;
	parity->failure_accounted = true;
	parity->q3n->raid_failed++;
	atomic64_inc(&parity->q3n->failed_stripes);
	atomic64_inc(&parity->q3n->unprotected_stripes);
	parity->q3n->parity_index[
		qemu_3dnand_parity_index(parity->q3n, parity->block,
					  parity->stripe)].valid = false;
}

static void qemu_3dnand_account_queue_failure(struct qemu_3dnand *q3n,
					       u32 block, u32 stripe)
{
	q3n->raid_failed++;
	atomic64_inc(&q3n->failed_stripes);
	atomic64_inc(&q3n->unprotected_stripes);
	q3n->parity_index[
		qemu_3dnand_parity_index(q3n, block, stripe)].valid = false;
}

static u64 qemu_3dnand_stripe_id(struct qemu_3dnand *q3n, u32 block,
				  u32 stripe)
{
	return (u64)block * q3n->pages_per_block + stripe;
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
					    const u8 *parity)
{
	int ret;

	ret = qemu_3dnand_program_phys_page_locked(q3n, block,
			stripe * Q3N_STRIPE_PAGES + Q3N_DATA_PAGES, parity,
			Q3N_OP_PARITY_WRITE);
	if (ret)
		return ret;

	entry->physical_block = block;
	entry->page = stripe * Q3N_STRIPE_PAGES + Q3N_DATA_PAGES;
	entry->data_block_generation[0] = q3n->data_meta[block].generation;
	entry->parity_version++;
	entry->sequence = ++q3n->parity_sequence;
	entry->valid = true;
	q3n->parity_written++;
	atomic64_inc(&q3n->protected_stripes);
	return 0;
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
		if (ret == -ESTALE) {
			parity->q3n->parity_stale++;
			goto out_finish;
		}
		goto out_failed;
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

	if (parity->rebuild.next_slot < Q3N_DATA_PAGES) {
		struct q3n_ecc_result ecc;
		u8 slot = parity->rebuild.next_slot;

		ret = qemu_3dnand_read_phys_page_locked(parity->q3n,
			parity->block,
			parity->stripe * Q3N_STRIPE_PAGES + slot,
			parity->page_buf, Q3N_OP_PARITY_READ, &ecc);
		if (!ret)
			qemu_3dnand_account_background_ecc(parity->q3n, &ecc);
		if (!ret && ecc.uncorrectable)
			ret = -EBADMSG;
		if (!ret)
			ret = q3n_rebuild_xor_one(&parity->rebuild,
						  parity->page_buf);
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
		}
	} else {
		entry = &parity->q3n->parity_index[
			qemu_3dnand_parity_index(parity->q3n, parity->block,
						  parity->stripe)];
		ret = qemu_3dnand_commit_parity_locked(parity->q3n,
			parity->block, parity->stripe, entry,
			parity->rebuild.parity_accumulator);
	}
	mutex_unlock(&parity->q3n->mtd_lock);
	if (ret)
		goto out_failed;
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
	mutex_lock(&parity->q3n->mtd_lock);
	qemu_3dnand_account_unprotected(parity);
	mutex_unlock(&parity->q3n->mtd_lock);
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
					   u32 data_block, u32 page, u8 *buf,
					   struct mtd_req_stats *stats,
					   struct q3n_ecc_result *ecc)
{
	struct q3n_ecc_result total = {};
	struct q3n_ecc_result source;
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
						Q3N_OP_FOREGROUND, &source);
	if (ret)
		return ret;
	if (source.uncorrectable)
		return -EBADMSG;
	qemu_3dnand_account_foreground_ecc(q3n, stats, &source);
	q3n->raid_source_corrected_bits += source.corrected_bits;
	q3n_ecc_accumulate(&total, &source);

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
							Q3N_OP_FOREGROUND,
							&source);
		if (ret)
			return ret;
		if (source.uncorrectable)
			return -EBADMSG;
		qemu_3dnand_account_foreground_ecc(q3n, stats, &source);
		q3n->raid_source_corrected_bits += source.corrected_bits;
		q3n_ecc_accumulate(&total, &source);
		for (i = 0; i < q3n->page_size; i++)
			buf[i] ^= q3n->page_buf[i];
	}

	*ecc = total;
	q3n->raid_recovered++;
	return 0;
}

static int qemu_3dnand_read_data_page_locked(struct qemu_3dnand *q3n,
					     u32 data_block, u32 page, u8 *buf,
					     struct mtd_req_stats *stats,
					     struct q3n_ecc_result *ecc)
{
	int ret;

	ret = qemu_3dnand_read_phys_page_locked(q3n, data_block, page, buf,
						Q3N_OP_FOREGROUND, ecc);
	if (!ret && ecc->uncorrectable)
		return -EBADMSG;
	if (!ret) {
		qemu_3dnand_account_foreground_ecc(q3n, stats, ecc);
		return 0;
	}

	ret = qemu_3dnand_recover_page_locked(q3n, data_block, page, buf,
					      stats, ecc);
	if (ret)
		q3n->raid_failed++;
	return ret;
}

static int qemu_3dnand_profile_group(struct qemu_3dnand *q3n, loff_t addr,
				     struct q3n_raid_group *group,
				     u64 *leb_out, u8 *data_slot,
				     u32 *column)
{
	u64 leb = div64_u64(addr, q3n->mtd->erasesize);
	u64 in_leb = addr % q3n->mtd->erasesize;
	u32 page;

	if (leb >= q3n->profile_leb_count)
		return -ERANGE;
	if (raid_level == Q3N_RAID1) {
		u32 page_column;

		page = div_u64_rem(in_leb, q3n->page_size, &page_column);
		if (column)
			*column = page_column;
		if (data_slot)
			*data_slot = 0;
		if (leb_out)
			*leb_out = leb;
		return q3n_map_raid1_page(&q3n->profile_geometry, leb, page,
					  group);
	}

	page = div_u64(in_leb, 3 * q3n->page_size);
	if (column)
		*column = in_leb % q3n->page_size;
	if (data_slot)
		*data_slot = div_u64(in_leb, q3n->page_size) % 3;
	if (leb_out)
		*leb_out = leb;
	return q3n_map_raid5_stripe(&q3n->profile_geometry, leb, page, group);
}

static void qemu_3dnand_profile_buffers(struct qemu_3dnand *q3n,
					const struct q3n_raid_group *group,
					struct q3n_mp_buffers *buffers,
					bool oob)
{
	u8 plane;

	memset(buffers, 0, sizeof(*buffers));
	buffers->mask = group->member_mask;
	buffers->die = group->die;
	for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
		buffers->addr[plane] = group->member[plane];
		buffers->data[plane] = oob ? q3n->mp_oob[plane] :
			q3n->mp_buf[plane];
	}
}

static int qemu_3dnand_profile_write_page(struct qemu_3dnand *q3n,
					  loff_t to, const u8 *data)
{
	struct q3n_mp_buffers buffers;
	struct q3n_mp_result result;
	struct q3n_raid_group group;
	u64 leb;
	u64 state_index;
	u32 column;
	u8 plane;
	int ret;

	ret = qemu_3dnand_profile_group(q3n, to, &group, &leb, NULL, &column);
	if (ret || column)
		return ret ?: -EINVAL;
	state_index = leb * q3n->pages_per_block + group.page;
	q3n_recovery_begin(&q3n->profile_state[state_index]);
	qemu_3dnand_profile_buffers(q3n, &group, &buffers, false);
	if (raid_level == Q3N_RAID1) {
		for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++)
			if (group.member_mask & BIT(plane))
				buffers.data[plane] = (u8 *)data;
	} else {
		u8 slot = 0;

		memset(q3n->raid_buf, 0, q3n->page_size);
		for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
			if (plane == group.parity_plane) {
				buffers.data[plane] = q3n->raid_buf;
				continue;
			}
			buffers.data[plane] = (u8 *)data + slot * q3n->page_size;
			q3n_xor_page(q3n->raid_buf, buffers.data[plane],
				     q3n->page_size);
			slot++;
		}
		q3n->last_parity_plane = group.parity_plane;
	}

	ret = q3n_mp_program(&q3n->mp, &buffers, &result);
	q3n_recovery_complete(&q3n->profile_state[state_index],
			      !ret && result.success_mask == group.member_mask);
	return !ret && result.success_mask == group.member_mask ? 0 : -EIO;
}

static void qemu_3dnand_profile_account_ecc(struct qemu_3dnand *q3n,
					    const struct q3n_ecc_result *ecc)
{
	q3n->mtd->ecc_stats.corrected += ecc->corrected_bits;
}

static void qemu_3dnand_profile_invalidate_leb(struct qemu_3dnand *q3n,
					       u64 leb)
{
	memset(&q3n->profile_state[leb * q3n->pages_per_block], 0,
	       q3n->pages_per_block * sizeof(*q3n->profile_state));
}

static int qemu_3dnand_profile_read_page(struct qemu_3dnand *q3n,
					 loff_t from, u8 *out)
{
	struct q3n_mp_buffers buffers;
	struct q3n_mp_result result;
	struct q3n_raid_group group;
	u64 leb;
	u64 state_index;
	u32 column;
	u32 max_bitflips = 0;
	int ret;

	ret = qemu_3dnand_profile_group(q3n, from, &group, &leb, NULL,
					&column);
	if (ret)
		return ret;
	if (column)
		return -EINVAL;
	state_index = leb * q3n->pages_per_block + group.page;

	qemu_3dnand_profile_buffers(q3n, &group, &buffers, false);
	ret = q3n_mp_read(&q3n->mp, &buffers, &result);
	if (ret && (ret != -EIO || !result.failure_mask ||
		    !result.success_mask))
		return ret;
	if (raid_level == Q3N_RAID1) {
		u8 first = (group.stripe_id & 1) ? fls(group.member_mask) - 1 :
			__ffs(group.member_mask);
		u8 second = first == __ffs(group.member_mask) ?
			fls(group.member_mask) - 1 : __ffs(group.member_mask);
		bool first_ok = (result.success_mask & BIT(first)) &&
			!result.ecc[first].uncorrectable;
		bool second_ok = (result.success_mask & BIT(second)) &&
			!result.ecc[second].uncorrectable;

		if (first_ok && second_ok &&
		    memcmp(q3n->mp_buf[first], q3n->mp_buf[second],
			   q3n->page_size))
			goto failed;
		if (first_ok) {
			memcpy(out, q3n->mp_buf[first], q3n->page_size);
			qemu_3dnand_profile_account_ecc(q3n, &result.ecc[first]);
			return result.ecc[first].max_bitflips;
		}
		if (second_ok &&
		    q3n_recovery_allowed(q3n->profile_state[state_index])) {
			memcpy(out, q3n->mp_buf[second], q3n->page_size);
			qemu_3dnand_profile_account_ecc(q3n, &result.ecc[second]);
			q3n->raid_recovered++;
			return max_t(u32, result.ecc[second].max_bitflips,
				     q3n->mtd->bitflip_threshold);
		}
	} else {
		u8 missing = Q3N_PLANES_PER_DIE;
		u8 failed = 0;
		u8 slot = 0;
		u8 plane;

		for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
			bool ok = (result.success_mask & BIT(plane)) &&
				!result.ecc[plane].uncorrectable;

			if (plane == group.parity_plane)
				continue;
			if (!ok) {
				failed++;
				missing = plane;
			} else {
				memcpy(out + slot * q3n->page_size,
				       q3n->mp_buf[plane], q3n->page_size);
				qemu_3dnand_profile_account_ecc(q3n,
							&result.ecc[plane]);
				max_bitflips = max(max_bitflips,
						   result.ecc[plane].max_bitflips);
			}
			slot++;
		}
		if (!failed)
			return max_bitflips;
		if (failed == 1 &&
		    (result.success_mask & BIT(group.parity_plane)) &&
		    !result.ecc[group.parity_plane].uncorrectable &&
		    q3n_recovery_allowed(q3n->profile_state[state_index])) {
			u8 missing_slot = missing < group.parity_plane ?
				missing : missing - 1;
			u8 source[2];
			u8 count = 0;

			for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++)
				if (plane != missing && plane != group.parity_plane)
					source[count++] = plane;
			q3n_raid5_recover(out + missing_slot * q3n->page_size,
				q3n->mp_buf[group.parity_plane],
				q3n->mp_buf[source[0]], q3n->mp_buf[source[1]],
				q3n->page_size);
			q3n->raid_recovered++;
			return max_t(u32, max_bitflips,
				     q3n->mtd->bitflip_threshold);
		}
	}

failed:
	q3n->mtd->ecc_stats.failed++;
	q3n->raid_failed++;
	return q3n->mtd->ecc_strength;
}

static int __maybe_unused qemu_3dnand_profile_read(struct mtd_info *mtd,
					    loff_t from, size_t len,
					    size_t *retlen, u_char *buf)
{
	struct qemu_3dnand *q3n = mtd->priv;
	size_t done = 0;
	int max_bitflips = 0;
	int ret = 0;

	*retlen = 0;
	if (from < 0 || from + len > mtd->size)
		return -EINVAL;
	mutex_lock(&q3n->mtd_lock);
	while (done < len) {
		u32 column = (from + done) % q3n->page_size;
		size_t chunk = min_t(size_t, len - done,
					 q3n->page_size - column);

		ret = qemu_3dnand_profile_read_page(q3n,
				(from + done) - column, q3n->page_buf);
		if (ret < 0)
			break;
		max_bitflips = max(max_bitflips, ret);
		memcpy(buf + done, q3n->page_buf + column, chunk);
		done += chunk;
	}
	mutex_unlock(&q3n->mtd_lock);
	*retlen = done;
	return ret < 0 ? ret : max_bitflips;
}

static int __maybe_unused qemu_3dnand_profile_write(struct mtd_info *mtd,
					     loff_t to, size_t len,
					     size_t *retlen,
					     const u_char *buf)
{
	struct qemu_3dnand *q3n = mtd->priv;
	size_t done = 0;
	int ret = 0;

	*retlen = 0;
	if (to < 0 || to + len > mtd->size || to % mtd->writesize ||
	    len % mtd->writesize)
		return -EINVAL;
	mutex_lock(&q3n->mtd_lock);
	while (done < len) {
		ret = qemu_3dnand_profile_write_page(q3n, to + done, buf + done);
		if (ret)
			break;
		done += mtd->writesize;
	}
	mutex_unlock(&q3n->mtd_lock);
	*retlen = done;
	return ret;
}

static int qemu_3dnand_mtd_read(struct mtd_info *mtd, loff_t from, size_t len,
				size_t *retlen, u_char *buf,
				struct mtd_req_stats *stats)
{
	struct qemu_3dnand *q3n = mtd->priv;
	struct q3n_ecc_result total = {};
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
		struct q3n_ecc_result page_ecc;
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
							q3n->page_buf, stats,
							&page_ecc);
		if (ret)
			break;
		q3n_ecc_accumulate(&total, &page_ecc);
		memcpy(buf + done, q3n->page_buf + column, chunk);
		done += chunk;
	}
	mutex_unlock(&q3n->mtd_lock);

	*retlen = done;
	return ret ?: q3n_ecc_result_to_mtd_ret(&total);
}

static int __maybe_unused qemu_3dnand_mtd_read_oob(struct mtd_info *mtd, loff_t from,
				     struct mtd_oob_ops *ops)
{
	struct qemu_3dnand *q3n = mtd->priv;
	struct q3n_ecc_result total = {};
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
					    &ops->retlen, ops->datbuf,
					    ops->stats);

	logical_page = div64_u64(from, q3n->page_size);
	column = from % q3n->page_size;
	ooboffs = ops->ooboffs;
	ret = qemu_3dnand_lock_request(q3n, &req);
	if (ret)
		return ret;
	while (data_done < ops->len || oob_done < ops->ooblen) {
		struct q3n_ecc_result ecc = {};
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
		if (data_chunk) {
			ret = qemu_3dnand_read_phys_page_locked(q3n, block, page,
				q3n->page_buf, Q3N_OP_FOREGROUND, &ecc);
			if (ret)
				break;
			if (ecc.uncorrectable) {
				ret = -EBADMSG;
				break;
			}
			qemu_3dnand_account_foreground_ecc(q3n, ops->stats, &ecc);
			q3n_ecc_accumulate(&total, &ecc);
			memcpy(ops->datbuf + data_done,
			       q3n->page_buf + column, data_chunk);
			data_done += data_chunk;
		}
		ret = qemu_3dnand_read_phys_oob_locked(q3n, block, page,
			logical_oob, Q3N_OP_FOREGROUND);
		if (ret)
			break;
		if (oob_chunk)
			memcpy(ops->oobbuf + oob_done, logical_oob + ooboffs,
			       oob_chunk);
		oob_done += oob_chunk;
		logical_page++;
		column = 0;
		ooboffs = 0;
	}
	mutex_unlock(&q3n->mtd_lock);

	ops->retlen = data_done;
	ops->oobretlen = oob_done;
	return ret ?: q3n_ecc_result_to_mtd_ret(&total);
}

static int qemu_3dnand_program_logical_page(struct qemu_3dnand *q3n,
					    u32 block, u32 page,
					    const u8 *data,
					    const u8 *oob, u32 ooboffs,
					    size_t ooblen, bool *data_programmed,
					    bool *oob_programmed)
{
	struct q3n_request req = {
		.class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_PROGRAM,
		.page = page,
	};
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	u32 stripe;
	bool parity_reserved = false;
	int ret;

	if (data_programmed)
		*data_programmed = false;
	if (oob_programmed)
		*oob_programmed = false;

	mutex_lock(&q3n->mtd_lock);
	ret = q3n->data_meta[block].bad ? -EIO : 0;
	mutex_unlock(&q3n->mtd_lock);
	if (ret)
		return ret;
	if (data && page % Q3N_STRIPE_PAGES == Q3N_DATA_PAGES - 1) {
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

	memset(logical_oob, 0xff, sizeof(logical_oob));
	ret = 0;
	if (ooblen)
		memcpy(logical_oob + ooboffs, oob, ooblen);
	if (!ret && data) {
		ret = qemu_3dnand_program_phys_page_locked(q3n, block, page,
			data, Q3N_OP_FOREGROUND);
		if (!ret && data_programmed)
			*data_programmed = true;
	}
	if (!ret && ooblen) {
		ret = qemu_3dnand_program_phys_oob_locked(q3n, block, page,
			logical_oob, Q3N_OP_FOREGROUND);
		if (!ret && oob_programmed)
			*oob_programmed = true;
	}
	if (ret)
		goto out_unlock;

	if (data) {
		q3n->data_page_valid[qemu_3dnand_data_page_index(q3n, block,
							      page)] = 1;
		stripe = page / Q3N_STRIPE_PAGES;
	}
	if (data && parity_reserved) {
		int parity_ret;

		parity_ret = qemu_3dnand_queue_parity_locked(q3n, block,
							stripe);
		if (parity_ret)
			qemu_3dnand_account_queue_failure(q3n, block, stripe);
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
							buf + done, NULL, 0, 0, NULL, NULL);
		if (ret)
			break;
		done += q3n->page_size;
	}

	*retlen = done;
	return ret;
}

static int __maybe_unused qemu_3dnand_mtd_write_oob(struct mtd_info *mtd, loff_t to,
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
		bool data_programmed;
		bool oob_programmed;
		u32 block, page, column;

		qemu_3dnand_decode_logical(q3n, page_addr, &block, &page,
					   &column);
		ret = qemu_3dnand_program_logical_page(q3n, block, page, data,
				ops->oobbuf + oob_done, ooboffs, oob_chunk,
				&data_programmed, &oob_programmed);
		if (data_programmed)
			data_done += q3n->page_size;
		if (oob_programmed)
			oob_done += oob_chunk;
		if (ret) {
			if (ret == -ESTALE)
				ret = -EIO;
			break;
		}
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

static int __maybe_unused qemu_3dnand_mtd_erase(struct mtd_info *mtd,
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
		q3n->generation_updates++;
		qemu_3dnand_invalidate_block_parity(q3n, block);
		q3n_block_cancel_end(&q3n->data_meta[block].parity_barrier);
		done += mtd->erasesize;
	}
	mutex_unlock(&q3n->mtd_lock);

	return ret;
}

static void __maybe_unused qemu_3dnand_mtd_sync(struct mtd_info *mtd)
{
	struct qemu_3dnand *q3n = mtd->priv;

	flush_workqueue(q3n->parity_wq);
	mutex_lock(&q3n->mtd_lock);
	q3n_sched_drain(&q3n->sched);
	mutex_unlock(&q3n->mtd_lock);
}

static int __maybe_unused qemu_3dnand_mtd_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	struct qemu_3dnand *q3n = mtd->priv;
	u64 block;
	u32 status;
	int ret;
	int bad;

	if (ofs < 0 || ofs >= mtd->size)
		return -EINVAL;
	block = div64_u64(ofs, mtd->erasesize);
	if (block >= q3n->data_block_count)
		return -EINVAL;

	mutex_lock(&q3n->mtd_lock);
	ret = qemu_3dnand_get_phys_block_status_locked(q3n, block, &status);
	if (ret) {
		bad = ret;
	} else {
		q3n->data_meta[block].bad = status & Q3N_BLOCK_STATUS_BAD;
		bad = q3n->data_meta[block].bad;
	}
	mutex_unlock(&q3n->mtd_lock);
	return bad;
}

static int __maybe_unused qemu_3dnand_mtd_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	struct qemu_3dnand *q3n = mtd->priv;
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	u64 block;
	u32 status;
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
	ret = qemu_3dnand_get_phys_block_status_locked(q3n, block, &status);
	if (ret)
		goto out_unlock;
	if (status & Q3N_BLOCK_STATUS_BAD) {
		q3n->data_meta[block].bad = true;
		ret = 0;
		goto out_unlock;
	}
	ret = qemu_3dnand_cancel_block_parity(q3n, block);
	if (ret)
		goto out_unlock;

	qemu_3dnand_build_bad_block_oob(logical_oob);
	ret = qemu_3dnand_program_phys_oob_locked(q3n, block, 0,
						   logical_oob, Q3N_OP_FOREGROUND);
	if (!ret)
		q3n->data_meta[block].bad = true;
	q3n_block_cancel_end(&q3n->data_meta[block].parity_barrier);

out_unlock:
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static int __maybe_unused qemu_3dnand_profile_erase(struct mtd_info *mtd,
					     struct erase_info *instr)
{
	struct qemu_3dnand *q3n = mtd->priv;
	u64 done = 0;
	int ret = 0;

	if (instr->addr + instr->len > mtd->size ||
	    instr->addr % mtd->erasesize || instr->len % mtd->erasesize)
		return -EINVAL;
	mutex_lock(&q3n->mtd_lock);
	while (done < instr->len) {
		struct q3n_mp_buffers buffers;
		struct q3n_mp_result result;
		struct q3n_raid_group group;
		u64 leb;
		u32 column;

		ret = qemu_3dnand_profile_group(q3n, instr->addr + done,
						&group, &leb, NULL, &column);
		if (!ret) {
			qemu_3dnand_profile_buffers(q3n, &group, &buffers, false);
			ret = q3n_mp_erase(&q3n->mp, &buffers, &result);
		}
		if (ret) {
			instr->fail_addr = instr->addr + done;
			break;
		}
		memset(&q3n->profile_state[leb * q3n->pages_per_block], 0,
		       q3n->pages_per_block * sizeof(*q3n->profile_state));
		done += mtd->erasesize;
	}
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static void __maybe_unused qemu_3dnand_profile_sync(struct mtd_info *mtd)
{
	(void)mtd;
}

static int __maybe_unused qemu_3dnand_profile_isbad(struct mtd_info *mtd,
					     loff_t ofs)
{
	struct qemu_3dnand *q3n = mtd->priv;
	struct q3n_mp_buffers buffers;
	struct q3n_mp_result result;
	struct q3n_raid_group group;
	u32 column;
	u8 plane;
	int ret;

	if (ofs < 0 || ofs >= mtd->size || ofs % mtd->erasesize)
		return -EINVAL;
	mutex_lock(&q3n->mtd_lock);
	ret = qemu_3dnand_profile_group(q3n, ofs, &group, NULL, NULL, &column);
	if (ret)
		goto out;
	qemu_3dnand_profile_buffers(q3n, &group, &buffers, true);
	q3n_mp_read_oob(&q3n->mp, &buffers, &result);
	for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
		if (!(group.member_mask & BIT(plane)))
			continue;
		if (!(result.success_mask & BIT(plane))) {
			ret = -EIO;
			goto out;
		}
		if (q3n->mp_oob[plane][0] != 0xff) {
			ret = 1;
			goto out;
		}
	}
	ret = 0;
out:
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static int __maybe_unused qemu_3dnand_profile_markbad(struct mtd_info *mtd,
					       loff_t ofs)
{
	struct qemu_3dnand *q3n = mtd->priv;
	struct q3n_mp_buffers buffers;
	struct q3n_mp_result result;
	struct q3n_raid_group group;
	u32 column;
	u8 plane;
	int ret;

	if (ofs < 0 || ofs >= mtd->size || ofs % mtd->erasesize)
		return -EINVAL;
	mutex_lock(&q3n->mtd_lock);
	ret = qemu_3dnand_profile_group(q3n, ofs, &group, NULL, NULL, &column);
	if (ret)
		goto out;
	qemu_3dnand_profile_buffers(q3n, &group, &buffers, true);
	q3n_mp_read_oob(&q3n->mp, &buffers, &result);
	for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
		if (!(group.member_mask & BIT(plane)))
			continue;
		if (!(result.success_mask & BIT(plane)))
			memset(q3n->mp_oob[plane], 0xff, Q3N_LOGICAL_OOB_SIZE);
		q3n->mp_oob[plane][0] = 0x00;
	}
	ret = q3n_mp_program_oob(&q3n->mp, &buffers, &result);
	if (result.success_mask != group.member_mask)
		ret = -EIO;
out:
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static struct qemu_3dnand *qemu_3dnand_from_chip(struct nand_chip *chip)
{
	return nand_get_controller_data(chip);
}

static int qemu_3dnand_ooblayout_ecc(struct mtd_info *mtd, int section,
				      struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;
	region->offset = mtd->oobsize;
	region->length = 0;
	return 0;
}

static int qemu_3dnand_ooblayout_free(struct mtd_info *mtd, int section,
				       struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;
	region->offset = mtd->oobsize;
	region->length = 0;
	return 0;
}

static const struct mtd_ooblayout_ops qemu_3dnand_ooblayout_ops = {
	.ecc = qemu_3dnand_ooblayout_ecc,
	.free = qemu_3dnand_ooblayout_free,
};

static int qemu_3dnand_ecc_read_oob(struct nand_chip *chip, int page)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int ret = 0;

	mutex_lock(&q3n->mtd_lock);
#if Q3N_ENABLE_MULTIPLANE_RAID
	{
		struct q3n_mp_buffers buffers;
		struct q3n_mp_result result;
		struct q3n_raid_group group;
		u8 plane;

		ret = qemu_3dnand_profile_group(q3n,
						 (loff_t)page * q3n->mtd->writesize,
						 &group, NULL, NULL, NULL);
		if (ret)
			goto out;
		qemu_3dnand_profile_buffers(q3n, &group, &buffers, true);
		ret = q3n_mp_read_oob(&q3n->mp, &buffers, &result);
		if (ret || result.success_mask != group.member_mask) {
			ret = ret ?: -EIO;
			goto out;
		}
		chip->oob_poi[0] = 0xff;
		for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++)
			if ((group.member_mask & BIT(plane)) &&
			    q3n->mp_oob[plane][Q3N_BBM_OOB_OFFSET] != 0xff)
				chip->oob_poi[0] = 0x00;
	}
#else
	{
		struct q3n_phys_addr phys;
		u8 oob[Q3N_LOGICAL_OOB_SIZE];

		ret = q3n_map_serial_data_page(&q3n->profile_geometry, page,
					       &phys);
		if (!ret)
			ret = qemu_3dnand_read_phys_oob_locked(q3n, phys.block,
							   phys.page, oob,
							   Q3N_OP_FOREGROUND);
		if (!ret)
			memcpy(chip->oob_poi, oob, q3n->mtd->oobsize);
	}
#endif
#if Q3N_ENABLE_MULTIPLANE_RAID
out:
#endif
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}

static int qemu_3dnand_ecc_write_oob(struct nand_chip *chip, int page)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	bool marker_written = false;
	int ret = 0;

	mutex_lock(&q3n->mtd_lock);
#if Q3N_ENABLE_MULTIPLANE_RAID
	{
		struct q3n_mp_buffers buffers;
		struct q3n_mp_result result;
		struct q3n_raid_group group;
		u64 leb;
		u8 plane;

		ret = qemu_3dnand_profile_group(q3n,
						 (loff_t)page * q3n->mtd->writesize,
						 &group, &leb, NULL, NULL);
		if (ret)
			goto out;
		qemu_3dnand_profile_invalidate_leb(q3n, leb);
		qemu_3dnand_profile_buffers(q3n, &group, &buffers, true);
		for (plane = 0; plane < Q3N_PLANES_PER_DIE; plane++) {
			if (!(group.member_mask & BIT(plane)))
				continue;
			memset(q3n->mp_oob[plane], 0xff, Q3N_LOGICAL_OOB_SIZE);
			q3n->mp_oob[plane][0] = chip->oob_poi[0];
		}
		ret = q3n_mp_program_oob(&q3n->mp, &buffers, &result);
		marker_written = result.success_mask != 0;
		if (result.success_mask != group.member_mask)
			ret = -EIO;
	}
#else
	{
		struct q3n_phys_addr phys;
		u8 oob[Q3N_LOGICAL_OOB_SIZE];

		memset(oob, 0xff, sizeof(oob));
		memcpy(oob, chip->oob_poi, q3n->mtd->oobsize);
		ret = q3n_map_serial_data_page(&q3n->profile_geometry, page,
					       &phys);
		if (!ret)
			ret = qemu_3dnand_program_phys_oob_locked(q3n, phys.block,
							      phys.page, oob,
							      Q3N_OP_FOREGROUND);
		marker_written = !ret;
	}
#endif
#if Q3N_ENABLE_MULTIPLANE_RAID
out:
#endif
	mutex_unlock(&q3n->mtd_lock);
	if (marker_written && chip->oob_poi[0] != 0xff && !q3n->core_markbad) {
		int bbt_ret = nand_bbt_markbad_from_oob(chip, page);

		if (bbt_ret && bbt_ret != -EOPNOTSUPP)
			return bbt_ret;
	}
	return ret;
}

static int qemu_3dnand_ecc_read_page(struct nand_chip *chip, u8 *buf,
				     int oob_required, int page)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int ret;

	mutex_lock(&q3n->mtd_lock);
#if Q3N_ENABLE_MULTIPLANE_RAID
	ret = qemu_3dnand_profile_read_page(q3n,
					   (loff_t)page * q3n->mtd->writesize,
					   buf);
#else
	{
		struct q3n_phys_addr phys;
		struct q3n_ecc_result ecc = {};

		ret = q3n_map_serial_data_page(&q3n->profile_geometry, page,
					       &phys);
		if (!ret)
			ret = qemu_3dnand_read_data_page_locked(q3n, phys.block,
							    phys.page, buf, NULL,
							    &ecc);
		if (!ret)
			ret = ecc.max_bitflips;
	}
#endif
	mutex_unlock(&q3n->mtd_lock);
	if (oob_required) {
		int oob_ret = qemu_3dnand_ecc_read_oob(chip, page);

		if (oob_ret && ret >= 0)
			ret = oob_ret;
	}
	if (ret == -EBADMSG) {
		q3n->mtd->ecc_stats.failed++;
		return q3n->mtd->ecc_strength;
	}
	return ret;
}

static int qemu_3dnand_ecc_write_page(struct nand_chip *chip, const u8 *buf,
				      int oob_required, int page)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int ret;

#if Q3N_ENABLE_MULTIPLANE_RAID
	mutex_lock(&q3n->mtd_lock);
	ret = qemu_3dnand_profile_write_page(q3n,
					    (loff_t)page * q3n->mtd->writesize,
					    buf);
	mutex_unlock(&q3n->mtd_lock);
#else
	{
		struct q3n_phys_addr phys;

		ret = q3n_map_serial_data_page(&q3n->profile_geometry, page,
					       &phys);
		if (!ret)
			ret = qemu_3dnand_program_logical_page(q3n, phys.block,
							   phys.page, buf, NULL,
							   0, 0, NULL, NULL);
	}
#endif
	if (!ret && oob_required)
		ret = qemu_3dnand_ecc_write_oob(chip, page);
	return ret;
}

static int qemu_3dnand_ecc_read_page_raw(struct nand_chip *chip, u8 *buf,
					 int oob_required, int page)
{
	return -EOPNOTSUPP;
}

static int qemu_3dnand_ecc_write_page_raw(struct nand_chip *chip,
					  const u8 *buf, int oob_required,
					  int page)
{
	return -EOPNOTSUPP;
}

static int qemu_3dnand_block_bad(struct nand_chip *chip, loff_t ofs)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int page = div64_u64(ofs, q3n->mtd->writesize);
	int ret;

	ret = qemu_3dnand_ecc_read_oob(chip, page);
	return ret ?: chip->oob_poi[0] != 0xff;
}

static int qemu_3dnand_block_markbad(struct nand_chip *chip, loff_t ofs)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int page = div64_u64(ofs, q3n->mtd->writesize);
	int ret;

	memset(chip->oob_poi, 0xff, q3n->mtd->oobsize);
	chip->oob_poi[0] = 0x00;
	q3n->core_markbad = true;
	ret = qemu_3dnand_ecc_write_oob(chip, page);
	q3n->core_markbad = false;
	return ret;
}

static int qemu_3dnand_erase_page(struct qemu_3dnand *q3n, u32 page)
{
#if Q3N_ENABLE_MULTIPLANE_RAID
	struct q3n_mp_buffers buffers;
	struct q3n_mp_result result;
	struct q3n_raid_group group;
	u64 leb;
	int ret;

	ret = qemu_3dnand_profile_group(q3n,
					 (loff_t)page * q3n->mtd->writesize,
					 &group, &leb, NULL, NULL);
	if (ret)
		return ret;
	qemu_3dnand_profile_invalidate_leb(q3n, leb);
	qemu_3dnand_profile_buffers(q3n, &group, &buffers, false);
	ret = q3n_mp_erase(&q3n->mp, &buffers, &result);
	if (!ret && result.success_mask != group.member_mask)
		ret = -EIO;
	return ret;
#else
	struct q3n_phys_addr phys;
	int ret;

	ret = q3n_map_serial_data_page(&q3n->profile_geometry, page, &phys);
	if (ret)
		return ret;
	ret = qemu_3dnand_cancel_block_parity(q3n, phys.block);
	if (ret)
		return ret;
	ret = qemu_3dnand_erase_phys_block_locked(q3n, phys.block);
	if (!ret) {
		memset(&q3n->data_page_valid[phys.block * q3n->pages_per_block],
		       0, q3n->pages_per_block);
		q3n->data_meta[phys.block].generation++;
		if (!q3n->data_meta[phys.block].generation)
			q3n->data_meta[phys.block].generation = 1;
		qemu_3dnand_invalidate_block_parity(q3n, phys.block);
	}
	q3n_block_cancel_end(&q3n->data_meta[phys.block].parity_barrier);
	return ret;
#endif
}

static void qemu_3dnand_cmdfunc(struct nand_chip *chip, unsigned int command,
				int column, int page_addr)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	static const u8 id[8] = { 0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44 };

	mutex_lock(&q3n->mtd_lock);
	q3n->legacy.len = 0;
	q3n->legacy.pos = 0;
	switch (command) {
	case NAND_CMD_READID:
		if (column)
			q3n->legacy.error = -EINVAL;
		else {
			memcpy(q3n->legacy.data, id, sizeof(id));
			q3n->legacy.len = sizeof(id);
		}
		break;
	case NAND_CMD_STATUS:
		q3n->legacy.data[0] = NAND_STATUS_READY | NAND_STATUS_WP |
			(q3n->legacy.error ? NAND_STATUS_FAIL : 0);
		q3n->legacy.len = 1;
		break;
	case NAND_CMD_RESET:
		qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_RESET);
		q3n->legacy.error = qemu_3dnand_wait_ready(q3n);
		q3n->legacy.erase_pending = false;
		break;
	case NAND_CMD_ERASE1:
		if (page_addr < 0 || page_addr %
					(q3n->mtd->erasesize / q3n->mtd->writesize))
			q3n->legacy.error = -EINVAL;
		else {
			q3n->legacy.erase_page = page_addr;
			q3n->legacy.erase_pending = true;
		}
		break;
	case NAND_CMD_ERASE2:
		if (!q3n->legacy.erase_pending)
			q3n->legacy.error = -EINVAL;
		else {
			q3n->legacy.erase_pending = false;
			q3n->legacy.error = qemu_3dnand_erase_page(q3n,
							q3n->legacy.erase_page);
		}
		break;
	case NAND_CMD_READ0:
	case NAND_CMD_READOOB:
	case NAND_CMD_SEQIN:
	case NAND_CMD_PAGEPROG:
		break;
	default:
		q3n->legacy.error = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&q3n->mtd_lock);
}

static int qemu_3dnand_waitfunc(struct nand_chip *chip)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);
	int ret;

	mutex_lock(&q3n->mtd_lock);
	ret = q3n->legacy.error;
	q3n->legacy.error = 0;
	mutex_unlock(&q3n->mtd_lock);
	return ret ?: NAND_STATUS_READY | NAND_STATUS_WP;
}

static u8 qemu_3dnand_read_byte(struct nand_chip *chip)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);

	if (q3n->legacy.pos >= q3n->legacy.len)
		return 0xff;
	return q3n->legacy.data[q3n->legacy.pos++];
}

static void qemu_3dnand_read_buf(struct nand_chip *chip, u8 *buf, int len)
{
	while (len-- > 0)
		*buf++ = qemu_3dnand_read_byte(chip);
}

static void qemu_3dnand_write_buf(struct nand_chip *chip, const u8 *buf,
				   int len)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);

	q3n->legacy.error = -EOPNOTSUPP;
}

static void qemu_3dnand_select_chip(struct nand_chip *chip, int chipnr)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);

	if (chipnr != 0 && chipnr != -1)
		q3n->legacy.error = -EINVAL;
}

static void qemu_3dnand_sync(struct nand_chip *chip)
{
	struct qemu_3dnand *q3n = qemu_3dnand_from_chip(chip);

	flush_workqueue(q3n->parity_wq);
	mutex_lock(&q3n->mtd_lock);
	q3n_sched_drain(&q3n->sched);
	mutex_unlock(&q3n->mtd_lock);
}

static int qemu_3dnand_attach_chip(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);

	mtd_set_ooblayout(mtd, &qemu_3dnand_ooblayout_ops);
	chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
	chip->ecc.placement = NAND_ECC_PLACEMENT_OOB;
	chip->ecc.size = Q3N_ECC_STEP_SIZE;
	chip->ecc.strength = Q3N_ECC_STRENGTH;
	chip->ecc.bytes = 0;
	chip->ecc.steps = mtd->writesize / Q3N_ECC_STEP_SIZE;
	chip->ecc.read_page = qemu_3dnand_ecc_read_page;
	chip->ecc.write_page = qemu_3dnand_ecc_write_page;
	chip->ecc.read_page_raw = qemu_3dnand_ecc_read_page_raw;
	chip->ecc.write_page_raw = qemu_3dnand_ecc_write_page_raw;
	chip->ecc.read_oob = qemu_3dnand_ecc_read_oob;
	chip->ecc.write_oob = qemu_3dnand_ecc_write_oob;
	chip->ecc.read_oob_raw = qemu_3dnand_ecc_read_oob;
	chip->ecc.write_oob_raw = qemu_3dnand_ecc_write_oob;
	if (!chip->ecc.steps)
		return -EINVAL;
	return 0;
}

static const struct nand_controller_ops qemu_3dnand_controller_ops = {
	.attach_chip = qemu_3dnand_attach_chip,
};

static int qemu_3dnand_register_mtd(struct qemu_3dnand *q3n)
{
	struct mtd_info *mtd;
	u32 writesize;
	u32 erasesize;
	u64 size;
	int ret;

#if Q3N_ENABLE_MULTIPLANE_RAID
	ret = q3n_raid_geometry_values(&q3n->profile_geometry, &writesize,
				       &erasesize, &size);
#else
	writesize = q3n->page_size;
	erasesize = (q3n->pages_per_block / Q3N_STRIPE_PAGES) *
		Q3N_DATA_PAGES * q3n->page_size;
	size = (u64)q3n->data_block_count * erasesize;
	ret = 0;
#endif
	if (ret || !writesize || !erasesize || !size || size > U64_MAX - SZ_1M)
		return ret ?: -EINVAL;

	q3n->scan_ids[0] = (struct nand_flash_dev) {
		.name = "QEMU 3D NAND logical array",
		.id = { 0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44 },
		.id_len = 8,
		.pagesize = writesize,
		.oobsize = Q3N_ENABLE_MULTIPLANE_RAID ? 1 : Q3N_LOGICAL_OOB_SIZE,
		.erasesize = erasesize,
		.chipsize = DIV_ROUND_UP_ULL(size, SZ_1M),
		.options = NAND_NO_SUBPAGE_WRITE | NAND_NON_POWER_OF_2_GEOMETRY |
			   NAND_MARKBAD_NO_ERASE,
		.ecc = NAND_ECC_INFO(Q3N_ECC_STRENGTH, Q3N_ECC_STEP_SIZE),
	};
	memset(&q3n->scan_ids[1], 0, sizeof(q3n->scan_ids[1]));

	nand_controller_init(&q3n->controller);
	q3n->controller.ops = &qemu_3dnand_controller_ops;
	q3n->chip.controller = &q3n->controller;
	nand_set_controller_data(&q3n->chip, q3n);
	q3n->chip.legacy.cmdfunc = qemu_3dnand_cmdfunc;
	q3n->chip.legacy.waitfunc = qemu_3dnand_waitfunc;
	q3n->chip.legacy.read_byte = qemu_3dnand_read_byte;
	q3n->chip.legacy.read_buf = qemu_3dnand_read_buf;
	q3n->chip.legacy.write_buf = qemu_3dnand_write_buf;
	q3n->chip.legacy.select_chip = qemu_3dnand_select_chip;
	q3n->chip.legacy.block_bad = qemu_3dnand_block_bad;
	q3n->chip.legacy.block_markbad = qemu_3dnand_block_markbad;
	q3n->chip.ops.sync = qemu_3dnand_sync;

	mtd = nand_to_mtd(&q3n->chip);
	q3n->mtd = mtd;
	mtd->name = "qemu-3dnand";
	mtd->dev.parent = &q3n->pdev->dev;
	mtd->owner = THIS_MODULE;
	mtd->bitflip_threshold = Q3N_ECC_STRENGTH;
	mtd->priv = q3n;

	ret = nand_scan_with_ids(&q3n->chip, 1, q3n->scan_ids);
	if (ret)
		return ret;
	q3n->scanned = true;
	if (mtd->size != size) {
		ret = -EINVAL;
		goto err_cleanup;
	}
	ret = mtd_device_register(mtd, NULL, 0);
	if (ret)
		goto err_cleanup;
	return 0;

err_cleanup:
	nand_cleanup(&q3n->chip);
	q3n->scanned = false;
	return ret;
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
	q3n->chip.pagecache.page = -1;
	mutex_unlock(&q3n->mtd_lock);

	return ret;
}

static int qemu_3dnand_inject_profile_plane_loss(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;
	u64 block;
	u64 addr;
	int ret;

	if (value >= Q3N_PLANES_PER_DIE)
		return -ERANGE;
	block = value * q3n->blocks_per_plane;
	addr = block * q3n->pages_per_block * q3n->page_size;
	mutex_lock(&q3n->mtd_lock);
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_LO, lower_32_bits(addr));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_ADDR_HI, upper_32_bits(addr));
	qemu_3dnand_writel(q3n, Q3N_REG_FAULT_CTRL,
			   Q3N_FAULT_INJECT_DATA_LOSS);
	ret = qemu_3dnand_wait_ready(q3n);
	q3n->chip.pagecache.page = -1;
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

	if (value >= q3n->mtd->size)
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
Q3N_MMIO_STAT_GETTER(multiplane_commands, Q3N_REG_STAT_MP_COMMANDS)
Q3N_MMIO_STAT_GETTER(multiplane_slot_failures,
		     Q3N_REG_STAT_MP_SLOT_FAILURES)
static int qemu_3dnand_raid_level_get(void *data, u64 *value)
{
	(void)data;
	*value = raid_level;
	return 0;
}

static int qemu_3dnand_metadata_degraded_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->metadata_degraded;
	return 0;
}

static int qemu_3dnand_last_parity_plane_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->last_parity_plane;
	return 0;
}
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

static int qemu_3dnand_raid_source_corrected_bits_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->raid_source_corrected_bits;
	return 0;
}

static int
qemu_3dnand_background_ecc_corrected_bits_get(void *data, u64 *value)
{
	struct qemu_3dnand *q3n = data;

	*value = q3n->background_ecc_corrected_bits;
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
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_inject_profile_plane_loss_fops, NULL,
	qemu_3dnand_inject_profile_plane_loss, "%llu\n");
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
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_multiplane_commands_fops,
	qemu_3dnand_multiplane_commands_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_multiplane_slot_failures_fops,
	qemu_3dnand_multiplane_slot_failures_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_raid_level_fops,
	qemu_3dnand_raid_level_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_metadata_degraded_fops,
	qemu_3dnand_metadata_degraded_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_last_parity_plane_fops,
	qemu_3dnand_last_parity_plane_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_raid_recovered_fops,
			 qemu_3dnand_raid_recovered_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_raid_failed_fops,
			 qemu_3dnand_raid_failed_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_raid_source_corrected_bits_fops,
			 qemu_3dnand_raid_source_corrected_bits_get, NULL,
			 "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(qemu_3dnand_background_ecc_corrected_bits_fops,
			 qemu_3dnand_background_ecc_corrected_bits_get, NULL,
			 "%llu\n");
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
	debugfs_create_file("inject_profile_plane_loss", 0200,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_inject_profile_plane_loss_fops);
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
	debugfs_create_file("raid_level", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_raid_level_fops);
	debugfs_create_file("multiplane_commands", 0400, q3n->debugfs_dir,
			    q3n, &qemu_3dnand_multiplane_commands_fops);
	debugfs_create_file("multiplane_slot_failures", 0400,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_multiplane_slot_failures_fops);
	debugfs_create_file("metadata_degraded", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_metadata_degraded_fops);
	debugfs_create_file("last_parity_plane", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_last_parity_plane_fops);
	debugfs_create_file("raid_recovered", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_raid_recovered_fops);
	debugfs_create_file("raid_failed", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_raid_failed_fops);
	debugfs_create_file("raid_source_corrected_bits", 0400,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_raid_source_corrected_bits_fops);
	debugfs_create_file("background_ecc_corrected_bits", 0400,
			    q3n->debugfs_dir, q3n,
			    &qemu_3dnand_background_ecc_corrected_bits_fops);
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
	u32 ecc_geom0;
	u32 ecc_geom1;
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

	ident = qemu_3dnand_readl(q3n, Q3N_REG_ID);
	if (ident != Q3N_ID_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected q3n id 0x%08x\n", ident);

	cap = qemu_3dnand_readl(q3n, Q3N_REG_CAP);
	q3n->cap = cap;
#if Q3N_ENABLE_MULTIPLANE_RAID
	if (raid_level != Q3N_RAID1 && raid_level != Q3N_RAID5)
		return dev_err_probe(dev, -EINVAL,
				     "raid_level must be 1 or 5\n");
	if (!(cap & Q3N_CAP_MULTIPLANE))
		return dev_err_probe(dev, -ENODEV,
				     "controller lacks multi-plane support\n");
#endif
	geom0 = qemu_3dnand_readl(q3n, Q3N_REG_GEOM0);
	geom1 = qemu_3dnand_readl(q3n, Q3N_REG_GEOM1);
	ecc_geom0 = qemu_3dnand_readl(q3n, Q3N_REG_ECC_GEOM0);
	ecc_geom1 = qemu_3dnand_readl(q3n, Q3N_REG_ECC_GEOM1);
	pool0 = qemu_3dnand_readl(q3n, Q3N_REG_POOL0);
	pool1 = qemu_3dnand_readl(q3n, Q3N_REG_POOL1);
	if ((ecc_geom0 & 0xffff) != Q3N_ECC_STEP_SIZE ||
	    (ecc_geom0 >> 16) != Q3N_ECC_STRENGTH ||
	    (ecc_geom1 & 0xffff) != Q3N_LDPC_BYTES_PER_STEP ||
	    (ecc_geom1 >> 16) != Q3N_LDPC_STEPS)
		return dev_err_probe(dev, -EINVAL,
			"unsupported ECC geometry %u/%u/%u/%u\n",
			ecc_geom0 & 0xffff, ecc_geom0 >> 16,
			ecc_geom1 & 0xffff, ecc_geom1 >> 16);

	q3n->page_size = geom0 & 0xffff;
	q3n->oob_size = geom0 >> 16;
	q3n->pages_per_block = geom1 & 0xffff;
	q3n->blocks_per_plane = geom1 >> 16;
	q3n->data_blocks_per_plane = pool0 & 0xffff;
	q3n->parity_blocks_per_plane = pool0 >> 16;
	q3n->metadata_blocks_per_plane = pool1 & 0xffff;
	q3n->reserve_blocks_per_plane = pool1 >> 16;
	q3n->mp.regs = q3n->regs;
	q3n->mp.page_size = q3n->page_size;
	q3n->mp.pages_per_block = q3n->pages_per_block;
	q3n->profile_geometry = (struct q3n_geometry) {
		.page_size = q3n->page_size,
		.pages_per_block = q3n->pages_per_block,
		.blocks_per_plane = q3n->blocks_per_plane,
		.data_blocks_per_plane = q3n->data_blocks_per_plane,
		.dies = Q3N_DIES,
		.planes_per_die = Q3N_PLANES_PER_DIE,
		.raid_level = raid_level,
	};
	q3n->profile_leb_count = q3n->data_blocks_per_plane *
		(raid_level == Q3N_RAID1 ? 4 : 2);
	q3n->last_parity_plane = Q3N_RAID_NO_PARITY;
	q3n->data_block_count = q3n->data_blocks_per_plane * Q3N_RAID_LANES;
	q3n->parity_block_count = q3n->parity_blocks_per_plane * Q3N_RAID_LANES;
	q3n->raid_group_count = q3n->data_block_count / Q3N_RAID_LANES;
	q3n->profile_geometry.data_pages_per_stripe = Q3N_DATA_PAGES;
	q3n->profile_geometry.data_block_count = q3n->data_block_count;
	q3n->profile_geometry.parity_block_count = q3n->parity_block_count;
	q3n->parity_wq = alloc_workqueue("q3n-parity", WQ_UNBOUND,
					 Q3N_MAX_PENDING_PARITY);
	if (!q3n->parity_wq)
		return -ENOMEM;
	pci_set_drvdata(pdev, q3n);

	q3n->page_buf = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
	q3n->raid_buf = devm_kmalloc(dev, q3n->page_size, GFP_KERNEL);
	for (ret = 0; ret < Q3N_PLANES_PER_DIE; ret++) {
		q3n->mp_buf[ret] = devm_kmalloc(dev, q3n->page_size,
						 GFP_KERNEL);
		q3n->mp_oob[ret] = devm_kmalloc(dev, Q3N_LOGICAL_OOB_SIZE,
						 GFP_KERNEL);
		if (!q3n->mp_buf[ret] || !q3n->mp_oob[ret]) {
			ret = -ENOMEM;
			goto err_free_metadata;
		}
	}
	q3n->profile_state = kvcalloc((u64)q3n->profile_leb_count *
				       q3n->pages_per_block,
				       sizeof(*q3n->profile_state), GFP_KERNEL);
	/* These arrays are multi-megabyte with the 2-die x 4-plane geometry. */
	q3n->data_page_valid = kvcalloc(q3n->data_block_count,
					  q3n->pages_per_block,
					  GFP_KERNEL);
	q3n->data_meta = devm_kcalloc(dev, q3n->data_block_count,
				      sizeof(*q3n->data_meta), GFP_KERNEL);
	q3n->parity_index = kvcalloc(q3n->data_block_count *
					     (q3n->pages_per_block /
					      Q3N_STRIPE_PAGES),
					     sizeof(*q3n->parity_index),
					     GFP_KERNEL);
	if (!q3n->page_buf || !q3n->raid_buf || !q3n->profile_state ||
	    !q3n->data_page_valid ||
	    !q3n->data_meta || !q3n->parity_index) {
		ret = -ENOMEM;
		goto err_free_metadata;
	}

	for (ret = 0; ret < q3n->data_block_count; ret++) {
		q3n->data_meta[ret].generation = 1;
		q3n_block_barrier_init(&q3n->data_meta[ret].parity_barrier);
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
		 "q3n driver %s: data=%u parity=%u metadata=%u reserve=%u bar=%pa size=%pa mtd=%s\n",
		 Q3N_ENABLE_MULTIPLANE_RAID ?
			(raid_level == Q3N_RAID1 ? "multi-plane RAID1" :
			 "multi-plane RAID5") : "serial RAID",
		 q3n->data_blocks_per_plane, q3n->parity_blocks_per_plane,
		 q3n->metadata_blocks_per_plane, q3n->reserve_blocks_per_plane,
		 &bar.start, &q3n->regs_size, q3n->mtd->name);

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
		mtd_device_unregister(q3n->mtd);
		flush_workqueue(q3n->parity_wq);
		if (q3n->scanned) {
			nand_cleanup(&q3n->chip);
			q3n->scanned = false;
		}
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
