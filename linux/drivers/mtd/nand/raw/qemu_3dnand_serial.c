// SPDX-License-Identifier: GPL-2.0
/* QEMU 3D NAND Linux driver — split by functional responsibility. */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/lockdep.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "qemu_3dnand_internal.h"

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

void qemu_3dnand_decode_logical(struct qemu_3dnand *q3n, loff_t addr,
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

	ret = q3n_hw_program_page_locked(q3n, block,
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

static void q3n_parity_pause_initial(struct qemu_3dnand_parity_work *parity)
{
	struct qemu_3dnand *q3n = parity->q3n;
	struct q3n_block_barrier *barrier =
		&q3n->data_meta[parity->block].parity_barrier;

	if (!READ_ONCE(q3n->parity_pause_enable) ||
	    READ_ONCE(q3n->parity_pause_block) != parity->block ||
	    q3n_block_is_cancelling(barrier))
		return;
	atomic_inc(&q3n->parity_paused);
	wait_event(q3n->parity_pause_waitq,
		   !READ_ONCE(q3n->parity_pause_enable) ||
		   READ_ONCE(q3n->parity_pause_block) != parity->block ||
		   q3n_block_is_cancelling(barrier));
	atomic_dec(&q3n->parity_paused);
}

static void q3n_parity_cancel_request_locked(
		struct qemu_3dnand_parity_work *parity)
{
	if (parity->request_queued)
		q3n_sched_cancel(&parity->q3n->sched, &parity->request);
	parity->request_queued = false;
}

static int q3n_parity_start_locked(struct qemu_3dnand_parity_work *parity,
				   u64 *sequence)
{
	struct qemu_3dnand *q3n = parity->q3n;
	struct q3n_block_barrier *barrier =
		&q3n->data_meta[parity->block].parity_barrier;
	int ret;

	if (q3n_block_is_cancelling(barrier)) {
		q3n_parity_cancel_request_locked(parity);
		return -ECANCELED;
	}
	ret = q3n_rebuild_check_generation(&parity->rebuild,
		q3n->data_meta[parity->block].generation);
	if (ret) {
		q3n_parity_cancel_request_locked(parity);
		return ret;
	}
	ret = q3n_sched_try_start_seq(&q3n->sched, &parity->request, sequence);
	if (!ret)
		parity->request_queued = false;
	return ret;
}

static int q3n_parity_rebuild_locked(struct qemu_3dnand_parity_work *parity)
{
	struct qemu_3dnand *q3n = parity->q3n;
	struct qemu_3dnand_parity_entry *entry;
	struct q3n_ecc_result ecc;
	u8 slot = parity->rebuild.next_slot;
	int ret;

	if (slot == Q3N_DATA_PAGES) {
		entry = &q3n->parity_index[qemu_3dnand_parity_index(q3n,
						parity->block, parity->stripe)];
		return qemu_3dnand_commit_parity_locked(q3n, parity->block,
			parity->stripe, entry, parity->rebuild.parity_accumulator);
	}
	ret = q3n_hw_read_page_locked(q3n, parity->block,
		parity->stripe * Q3N_STRIPE_PAGES + slot, parity->page_buf,
		Q3N_OP_PARITY_READ, &ecc);
	if (!ret)
		q3n_hw_account_background_ecc(q3n, &ecc);
	if (!ret && ecc.uncorrectable)
		ret = -EBADMSG;
	if (!ret)
		ret = q3n_rebuild_xor_one(&parity->rebuild, parity->page_buf);
	if (ret)
		return ret;
	if (parity->rebuild.next_slot < Q3N_DATA_PAGES)
		ret = q3n_sched_requeue_p1(&q3n->sched, &parity->request);
	else {
		parity->request.class = Q3N_REQ_PARITY_WRITE;
		parity->request.op = Q3N_REQ_PROGRAM;
		ret = q3n_sched_enqueue(&q3n->sched, &parity->request);
	}
	parity->request_queued = !ret;
	return ret;
}

static void q3n_parity_pause_continuation(
		struct qemu_3dnand_parity_work *parity)
{
	struct qemu_3dnand *q3n = parity->q3n;
	struct q3n_block_barrier *barrier =
		&q3n->data_meta[parity->block].parity_barrier;

	if (!READ_ONCE(q3n->parity_continuation_pause_enable) ||
	    READ_ONCE(q3n->parity_continuation_pause_block) != parity->block ||
	    READ_ONCE(q3n->parity_continuation_pause_class) !=
		parity->request.class || q3n_block_is_cancelling(barrier))
		return;
	atomic_inc(&q3n->parity_continuation_paused);
	wait_event(q3n->parity_pause_waitq,
		   !READ_ONCE(q3n->parity_continuation_pause_enable) ||
		   READ_ONCE(q3n->parity_continuation_pause_block) !=
			parity->block ||
		   READ_ONCE(q3n->parity_continuation_pause_class) !=
			parity->request.class || q3n_block_is_cancelling(barrier));
	atomic_dec(&q3n->parity_continuation_paused);
}

static void q3n_parity_fail(struct qemu_3dnand_parity_work *parity)
{
	mutex_lock(&parity->q3n->mtd_lock);
	qemu_3dnand_account_unprotected(parity);
	mutex_unlock(&parity->q3n->mtd_lock);
}

static void qemu_3dnand_parity_worker(struct work_struct *work)
{
	struct qemu_3dnand_parity_work *parity =
		container_of(work, struct qemu_3dnand_parity_work, work);
	u64 sequence;
	int ret;

	q3n_parity_pause_initial(parity);

again:
	if (!parity->request_queued) {
		ret = q3n_sched_requeue_p1(&parity->q3n->sched,
					   &parity->request);
		if (ret)
			goto out_failed;
		parity->request_queued = true;
	}

	mutex_lock(&parity->q3n->mtd_lock);
	ret = q3n_parity_start_locked(parity, &sequence);
	if (!ret)
		ret = q3n_parity_rebuild_locked(parity);
	mutex_unlock(&parity->q3n->mtd_lock);
	if (ret == -ECANCELED) {
		qemu_3dnand_finish_parity_work(parity);
		return;
	}
	if (ret == -EAGAIN) {
		q3n_sched_wait_for_change(&parity->q3n->sched, sequence);
		goto again;
	}
	if (ret == -ESTALE) {
		parity->q3n->parity_stale++;
		goto out_finish;
	}
	if (ret)
		goto out_failed;
	if (ret)
		goto out_failed;
	if (parity->rebuild.next_slot == Q3N_DATA_PAGES &&
	    !parity->request_queued)
		goto out_finish;
	q3n_parity_pause_continuation(parity);
	cond_resched();
	goto again;

out_failed:
	q3n_parity_fail(parity);
out_finish:
	qemu_3dnand_finish_parity_work(parity);
}

static int q3n_parity_init_work(struct qemu_3dnand_parity_work *parity,
				struct qemu_3dnand *q3n, u32 block,
				u32 stripe)
{
	parity->q3n = q3n;
	parity->block = block;
	parity->stripe = stripe;
	parity->rebuild.parity_accumulator = kzalloc(q3n->page_size, GFP_KERNEL);
	parity->page_buf = kmalloc(q3n->page_size, GFP_KERNEL);
	if (!parity->rebuild.parity_accumulator || !parity->page_buf)
		return -ENOMEM;
	parity->rebuild.stripe_id = qemu_3dnand_stripe_id(q3n, block, stripe);
	parity->rebuild.generation = q3n->data_meta[block].generation;
	parity->rebuild.data_pages = Q3N_DATA_PAGES;
	parity->rebuild.page_size = q3n->page_size;
	parity->request.class = Q3N_REQ_PARITY_READ;
	parity->request.op = Q3N_REQ_READ;
	parity->request.page = stripe * Q3N_STRIPE_PAGES + Q3N_DATA_PAGES;
	INIT_WORK(&parity->work, qemu_3dnand_parity_worker);
	return 0;
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
	ret = q3n_parity_init_work(parity, q3n, block, stripe);
	if (ret) {
		qemu_3dnand_finish_parity_work(parity);
		return ret;
	}
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

	ret = q3n_hw_read_page_locked(q3n, entry->physical_block,
						entry->page, buf,
						Q3N_OP_FOREGROUND, &source);
	if (ret)
		return ret;
	if (source.uncorrectable)
		return -EBADMSG;
	q3n_hw_account_foreground_ecc(q3n, stats, &source);
	q3n->raid_source_corrected_bits += source.corrected_bits;
	q3n_ecc_accumulate(&total, &source);

	for (lane = 0; lane < Q3N_DATA_PAGES; lane++) {
		if (lane == missing_lane)
			continue;
		if (!q3n->data_page_valid[qemu_3dnand_data_page_index(q3n,
							       data_block,
							       stripe * Q3N_STRIPE_PAGES + lane)])
			return -EIO;
		ret = q3n_hw_read_page_locked(q3n, data_block,
					 stripe * Q3N_STRIPE_PAGES + lane,
							q3n->page_buf,
							Q3N_OP_FOREGROUND,
							&source);
		if (ret)
			return ret;
		if (source.uncorrectable)
			return -EBADMSG;
		q3n_hw_account_foreground_ecc(q3n, stats, &source);
		q3n->raid_source_corrected_bits += source.corrected_bits;
		q3n_ecc_accumulate(&total, &source);
		for (i = 0; i < q3n->page_size; i++)
			buf[i] ^= q3n->page_buf[i];
	}

	*ecc = total;
	q3n->raid_recovered++;
	return 0;
}

int q3n_serial_read_page_locked(struct qemu_3dnand *q3n,
					     u32 data_block, u32 page, u8 *buf,
					     struct mtd_req_stats *stats,
					     struct q3n_ecc_result *ecc)
{
	int ret;

	ret = q3n_hw_read_page_locked(q3n, data_block, page, buf,
						Q3N_OP_FOREGROUND, ecc);
	if (!ret && ecc->uncorrectable)
		return -EBADMSG;
	if (!ret) {
		q3n_hw_account_foreground_ecc(q3n, stats, ecc);
		return 0;
	}

	ret = qemu_3dnand_recover_page_locked(q3n, data_block, page, buf,
					      stats, ecc);
	if (ret)
		q3n->raid_failed++;
	return ret;
}

int q3n_serial_mtd_read(struct mtd_info *mtd, loff_t from, size_t len,
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
		ret = q3n_serial_read_page_locked(q3n, block, page,
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

struct q3n_serial_oob_cursor {
	size_t data_done;
	size_t oob_done;
	u64 logical_page;
	u32 column;
	u32 ooboffs;
};

static int q3n_serial_validate_oob(struct mtd_info *mtd, loff_t addr,
				   struct mtd_oob_ops *ops)
{
	if (!ops)
		return -EINVAL;
	ops->retlen = 0;
	ops->oobretlen = 0;
	if (ops->mode != MTD_OPS_PLACE_OOB && ops->mode != MTD_OPS_RAW)
		return -EOPNOTSUPP;
	if (addr < 0 || addr + ops->len > mtd->size)
		return -EINVAL;
	if (ops->ooboffs >= Q3N_LOGICAL_OOB_SIZE && ops->ooblen)
		return -EINVAL;
	return 0;
}

static int q3n_serial_read_oob_step(struct qemu_3dnand *q3n,
		struct mtd_oob_ops *ops, struct q3n_serial_oob_cursor *cursor,
		struct q3n_ecc_result *total)
{
	struct q3n_ecc_result ecc = {};
	loff_t page_addr = cursor->logical_page * q3n->page_size;
	size_t data_chunk = min_t(size_t, ops->len - cursor->data_done,
				  q3n->page_size - cursor->column);
	size_t oob_chunk = min_t(size_t, ops->ooblen - cursor->oob_done,
				 Q3N_LOGICAL_OOB_SIZE - cursor->ooboffs);
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	u32 block, page, ignored;
	int ret;

	qemu_3dnand_decode_logical(q3n, page_addr, &block, &page, &ignored);
	if (q3n_block_is_cancelling(&q3n->data_meta[block].parity_barrier))
		return -EBUSY;
	if (data_chunk) {
		ret = q3n_hw_read_page_locked(q3n, block, page, q3n->page_buf,
					      Q3N_OP_FOREGROUND, &ecc);
		if (ret || ecc.uncorrectable)
			return ret ?: -EBADMSG;
		q3n_hw_account_foreground_ecc(q3n, ops->stats, &ecc);
		q3n_ecc_accumulate(total, &ecc);
		memcpy(ops->datbuf + cursor->data_done,
		       q3n->page_buf + cursor->column, data_chunk);
		cursor->data_done += data_chunk;
	}
	ret = q3n_hw_read_oob_locked(q3n, block, page, logical_oob,
				     Q3N_OP_FOREGROUND);
	if (!ret && oob_chunk)
		memcpy(ops->oobbuf + cursor->oob_done,
		       logical_oob + cursor->ooboffs, oob_chunk);
	if (ret)
		return ret;
	cursor->oob_done += oob_chunk;
	cursor->logical_page++;
	cursor->column = 0;
	cursor->ooboffs = 0;
	return 0;
}

int __maybe_unused q3n_serial_mtd_read_oob(struct mtd_info *mtd, loff_t from,
				     struct mtd_oob_ops *ops)
{
	struct qemu_3dnand *q3n = mtd->priv;
	struct q3n_ecc_result total = {};
	struct q3n_request req = {
		.class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_READ,
	};
	struct q3n_serial_oob_cursor cursor = {};
	int ret = 0;

	ret = q3n_serial_validate_oob(mtd, from, ops);
	if (ret)
		return ret;
	if (!ops->oobbuf)
		return q3n_serial_mtd_read(mtd, from, ops->len,
					    &ops->retlen, ops->datbuf,
					    ops->stats);

	cursor.logical_page = div64_u64(from, q3n->page_size);
	cursor.column = from % q3n->page_size;
	cursor.ooboffs = ops->ooboffs;
	ret = qemu_3dnand_lock_request(q3n, &req);
	if (ret)
		return ret;
	while (cursor.data_done < ops->len || cursor.oob_done < ops->ooblen)
		if ((ret = q3n_serial_read_oob_step(q3n, ops, &cursor, &total)))
			break;
	mutex_unlock(&q3n->mtd_lock);

	ops->retlen = cursor.data_done;
	ops->oobretlen = cursor.oob_done;
	return ret ?: q3n_ecc_result_to_mtd_ret(&total);
}

static int q3n_serial_check_block_locked(struct qemu_3dnand *q3n, u32 block)
{
	if (q3n_block_is_cancelling(&q3n->data_meta[block].parity_barrier))
		return -EBUSY;
	return q3n->data_meta[block].bad ? -EIO : 0;
}

static int q3n_serial_program_buffers_locked(struct qemu_3dnand *q3n,
		u32 block, u32 page, const u8 *data, const u8 *oob, u32 ooboffs,
		size_t ooblen, bool *data_programmed, bool *oob_programmed)
{
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	int ret = 0;

	memset(logical_oob, 0xff, sizeof(logical_oob));
	if (ooblen)
		memcpy(logical_oob + ooboffs, oob, ooblen);
	if (data) {
		ret = q3n_hw_program_page_locked(q3n, block, page, data,
						 Q3N_OP_FOREGROUND);
		if (!ret && data_programmed)
			*data_programmed = true;
	}
	if (!ret && ooblen) {
		ret = q3n_hw_program_oob_locked(q3n, block, page, logical_oob,
						Q3N_OP_FOREGROUND);
		if (!ret && oob_programmed)
			*oob_programmed = true;
	}
	return ret;
}

static void q3n_serial_finish_data_locked(struct qemu_3dnand *q3n, u32 block,
					  u32 page, bool parity_reserved)
{
	u32 stripe;
	int ret;

	q3n->data_page_valid[qemu_3dnand_data_page_index(q3n, block, page)] = 1;
	if (!parity_reserved)
		return;
	stripe = page / Q3N_STRIPE_PAGES;
	ret = qemu_3dnand_queue_parity_locked(q3n, block, stripe);
	if (ret)
		qemu_3dnand_account_queue_failure(q3n, block, stripe);
}

int q3n_serial_write_page(struct qemu_3dnand *q3n,
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
	ret = q3n_serial_check_block_locked(q3n, block);
	if (ret)
		goto out_unlock;
	ret = q3n_serial_program_buffers_locked(q3n, block, page, data, oob,
			ooboffs, ooblen, data_programmed, oob_programmed);
	if (ret)
		goto out_unlock;
	if (data)
		q3n_serial_finish_data_locked(q3n, block, page, parity_reserved);
	mutex_unlock(&q3n->mtd_lock);
	return 0;

out_unlock:
	mutex_unlock(&q3n->mtd_lock);
out_release_parity:
	if (parity_reserved)
		q3n_sched_release_parity(&q3n->sched);
	return ret;
}

int q3n_serial_mtd_write(struct mtd_info *mtd, loff_t to, size_t len,
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
		ret = q3n_serial_write_page(q3n, block, page,
							buf + done, NULL, 0, 0, NULL, NULL);
		if (ret)
			break;
		done += q3n->page_size;
	}

	*retlen = done;
	return ret;
}

static int q3n_serial_write_oob_step(struct qemu_3dnand *q3n,
		struct mtd_oob_ops *ops, struct q3n_serial_oob_cursor *cursor)
{
	loff_t page_addr = cursor->logical_page * q3n->page_size;
	size_t oob_chunk = min_t(size_t, ops->ooblen - cursor->oob_done,
				 Q3N_LOGICAL_OOB_SIZE - cursor->ooboffs);
	const u8 *data = cursor->data_done < ops->len ?
		ops->datbuf + cursor->data_done : NULL;
	bool data_programmed;
	bool oob_programmed;
	u32 block, page, column;
	int ret;

	qemu_3dnand_decode_logical(q3n, page_addr, &block, &page, &column);
	ret = q3n_serial_write_page(q3n, block, page, data,
		ops->oobbuf + cursor->oob_done, cursor->ooboffs, oob_chunk,
		&data_programmed, &oob_programmed);
	if (data_programmed)
		cursor->data_done += q3n->page_size;
	if (oob_programmed)
		cursor->oob_done += oob_chunk;
	if (ret)
		return ret == -ESTALE ? -EIO : ret;
	cursor->logical_page++;
	cursor->ooboffs = 0;
	return 0;
}

int __maybe_unused q3n_serial_mtd_write_oob(struct mtd_info *mtd, loff_t to,
				      struct mtd_oob_ops *ops)
{
	struct qemu_3dnand *q3n = mtd->priv;
	struct q3n_serial_oob_cursor cursor = {};
	int ret = 0;

	ret = q3n_serial_validate_oob(mtd, to, ops);
	if (ret)
		return ret;
	if (!ops->oobbuf)
		return q3n_serial_mtd_write(mtd, to, ops->len,
					     &ops->retlen, ops->datbuf);
	if (ops->datbuf &&
	    (!IS_ALIGNED(to, q3n->page_size) ||
	     !IS_ALIGNED(ops->len, q3n->page_size)))
		return -EINVAL;

	cursor.logical_page = div64_u64(to, q3n->page_size);
	cursor.ooboffs = ops->ooboffs;
	while (cursor.data_done < ops->len || cursor.oob_done < ops->ooblen)
		if ((ret = q3n_serial_write_oob_step(q3n, ops, &cursor)))
			break;

	ops->retlen = cursor.data_done;
	ops->oobretlen = cursor.oob_done;
	return ret;
}

void q3n_serial_invalidate_block_parity(struct qemu_3dnand *q3n,
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

int q3n_serial_cancel_block_parity(struct qemu_3dnand *q3n,
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

int __maybe_unused q3n_serial_mtd_erase(struct mtd_info *mtd,
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
		ret = q3n_serial_cancel_block_parity(q3n, block);
		if (ret) {
			instr->fail_addr = instr->addr + done;
			break;
		}
		ret = q3n_hw_erase_block_locked(q3n, block);
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
		q3n_serial_invalidate_block_parity(q3n, block);
		q3n_block_cancel_end(&q3n->data_meta[block].parity_barrier);
		done += mtd->erasesize;
	}
	mutex_unlock(&q3n->mtd_lock);

	return ret;
}

void __maybe_unused q3n_serial_sync(struct mtd_info *mtd)
{
	struct qemu_3dnand *q3n = mtd->priv;

	flush_workqueue(q3n->parity_wq);
	mutex_lock(&q3n->mtd_lock);
	q3n_sched_drain(&q3n->sched);
	mutex_unlock(&q3n->mtd_lock);
}

int __maybe_unused q3n_serial_block_bad(struct mtd_info *mtd, loff_t ofs)
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
	ret = q3n_hw_get_block_status_locked(q3n, block, &status);
	if (ret) {
		bad = ret;
	} else {
		q3n->data_meta[block].bad = status & Q3N_BLOCK_STATUS_BAD;
		bad = q3n->data_meta[block].bad;
	}
	mutex_unlock(&q3n->mtd_lock);
	return bad;
}

int __maybe_unused q3n_serial_block_markbad(struct mtd_info *mtd, loff_t ofs)
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
	ret = q3n_hw_get_block_status_locked(q3n, block, &status);
	if (ret)
		goto out_unlock;
	if (status & Q3N_BLOCK_STATUS_BAD) {
		q3n->data_meta[block].bad = true;
		ret = 0;
		goto out_unlock;
	}
	ret = q3n_serial_cancel_block_parity(q3n, block);
	if (ret)
		goto out_unlock;

	q3n_hw_build_bad_block_oob(logical_oob);
	ret = q3n_hw_program_oob_locked(q3n, block, 0,
						   logical_oob, Q3N_OP_FOREGROUND);
	if (!ret)
		q3n->data_meta[block].bad = true;
	q3n_block_cancel_end(&q3n->data_meta[block].parity_barrier);

out_unlock:
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}
