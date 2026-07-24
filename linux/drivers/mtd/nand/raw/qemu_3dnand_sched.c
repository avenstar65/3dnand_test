// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/module.h>

#include "qemu_3dnand_priv.h"

static struct list_head *q3n_sched_queue(struct q3n_sched *sched,
					 enum q3n_req_class class)
{
	switch (class) {
	case Q3N_REQ_FOREGROUND:
		return &sched->foreground_queue;
	case Q3N_REQ_PARITY_READ:
		return &sched->parity_read_queue;
	case Q3N_REQ_PARITY_WRITE:
		return &sched->parity_write_queue;
	default:
		return NULL;
	}
}

static void q3n_sched_remove_locked(struct q3n_sched *sched,
				    struct q3n_request *req)
{
	list_del_init(&req->node);
	if (req->class != Q3N_REQ_FOREGROUND)
		sched->pending_parity--;
}

static void q3n_sched_changed_locked(struct q3n_sched *sched)
{
	atomic64_inc(&sched->sequence);
}

static void q3n_sched_wake(struct q3n_sched *sched)
{
	wake_up_all(&sched->waitq);
}

void q3n_block_barrier_init(struct q3n_block_barrier *barrier)
{
	atomic_set(&barrier->pending_parity, 0);
	barrier->cancelling = false;
}

int q3n_block_parity_get(struct q3n_block_barrier *barrier)
{
	if (barrier->cancelling)
		return -EBUSY;
	atomic_inc(&barrier->pending_parity);
	return 0;
}

bool q3n_block_parity_put(struct q3n_block_barrier *barrier)
{
	return atomic_dec_and_test(&barrier->pending_parity);
}

int q3n_block_cancel_begin(struct q3n_block_barrier *barrier)
{
	if (barrier->cancelling)
		return -EBUSY;
	WRITE_ONCE(barrier->cancelling, true);
	return 0;
}

void q3n_block_cancel_end(struct q3n_block_barrier *barrier)
{
	WRITE_ONCE(barrier->cancelling, false);
}

bool q3n_block_is_cancelling(const struct q3n_block_barrier *barrier)
{
	return READ_ONCE(barrier->cancelling);
}

int q3n_block_pending(const struct q3n_block_barrier *barrier)
{
	return atomic_read(&barrier->pending_parity);
}

void q3n_sched_init(struct q3n_sched *sched)
{
	spin_lock_init(&sched->lock);
	init_waitqueue_head(&sched->waitq);
	atomic64_set(&sched->sequence, 0);
	INIT_LIST_HEAD(&sched->foreground_queue);
	INIT_LIST_HEAD(&sched->parity_read_queue);
	INIT_LIST_HEAD(&sched->parity_write_queue);
	sched->pending_parity = 0;
	sched->reserved_parity = 0;
	sched->max_pending_parity = 0;
	sched->p1_over_p2 = 0;
}

int q3n_sched_enqueue(struct q3n_sched *sched, struct q3n_request *req)
{
	struct list_head *queue;
	unsigned long flags;

	if (!sched || !req)
		return -EINVAL;
	queue = q3n_sched_queue(sched, req->class);
	if (!queue || req->op > Q3N_REQ_PROGRAM)
		return -EINVAL;

	spin_lock_irqsave(&sched->lock, flags);
	INIT_LIST_HEAD(&req->node);
	list_add_tail(&req->node, queue);
	if (req->class != Q3N_REQ_FOREGROUND) {
		sched->pending_parity++;
		if (sched->pending_parity > sched->max_pending_parity)
			sched->max_pending_parity = sched->pending_parity;
	}
	q3n_sched_changed_locked(sched);
	spin_unlock_irqrestore(&sched->lock, flags);
	q3n_sched_wake(sched);
	return 0;
}

int q3n_sched_cancel(struct q3n_sched *sched, struct q3n_request *req)
{
	unsigned long flags;

	if (!sched || !req)
		return -EINVAL;

	spin_lock_irqsave(&sched->lock, flags);
	if (list_empty(&req->node)) {
		spin_unlock_irqrestore(&sched->lock, flags);
		return -ENOENT;
	}
	q3n_sched_remove_locked(sched, req);
	q3n_sched_changed_locked(sched);
	spin_unlock_irqrestore(&sched->lock, flags);
	q3n_sched_wake(sched);
	return 0;
}

int q3n_sched_requeue_p1(struct q3n_sched *sched, struct q3n_request *req)
{
	if (!req)
		return -EINVAL;
	req->class = Q3N_REQ_PARITY_READ;
	return q3n_sched_enqueue(sched, req);
}

int q3n_sched_reserve_parity(struct q3n_sched *sched)
{
	unsigned long flags;
	int ret = 0;

	if (!sched)
		return -EINVAL;

	spin_lock_irqsave(&sched->lock, flags);
	if (sched->reserved_parity >= Q3N_MAX_PENDING_PARITY)
		ret = -ENOSPC;
	else
		sched->reserved_parity++;
	spin_unlock_irqrestore(&sched->lock, flags);
	return ret;
}

void q3n_sched_release_parity(struct q3n_sched *sched)
{
	unsigned long flags;

	if (!sched)
		return;

	spin_lock_irqsave(&sched->lock, flags);
	if (sched->reserved_parity)
		sched->reserved_parity--;
	spin_unlock_irqrestore(&sched->lock, flags);
}

void q3n_sched_get_counts(struct q3n_sched *sched, u32 *pending,
			  u32 *reserved, u32 *max_pending)
{
	unsigned long flags;

	spin_lock_irqsave(&sched->lock, flags);
	*pending = sched->pending_parity;
	*reserved = sched->reserved_parity;
	if (max_pending)
		*max_pending = sched->max_pending_parity;
	spin_unlock_irqrestore(&sched->lock, flags);
}

u64 q3n_sched_get_p1_over_p2(struct q3n_sched *sched)
{
	unsigned long flags;
	u64 value;

	spin_lock_irqsave(&sched->lock, flags);
	value = sched->p1_over_p2;
	spin_unlock_irqrestore(&sched->lock, flags);
	return value;
}

int q3n_sched_try_start_seq(struct q3n_sched *sched, struct q3n_request *req,
			    u64 *sequence)
{
	struct q3n_request *next = NULL;
	unsigned long flags;
	int ret = 0;

	if (!sched || !req)
		return -EINVAL;

	spin_lock_irqsave(&sched->lock, flags);
	next = list_first_entry_or_null(&sched->foreground_queue,
					struct q3n_request, node);
	if (!next)
		next = list_first_entry_or_null(&sched->parity_read_queue,
						struct q3n_request, node);
	if (!next)
		next = list_first_entry_or_null(&sched->parity_write_queue,
						struct q3n_request, node);
	if (next != req) {
		if (next && next->class == Q3N_REQ_PARITY_READ &&
		    req->class == Q3N_REQ_PARITY_WRITE)
			sched->p1_over_p2++;
		if (sequence)
			*sequence = atomic64_read(&sched->sequence);
		spin_unlock_irqrestore(&sched->lock, flags);
		return -EAGAIN;
	}

	q3n_sched_remove_locked(sched, req);

	q3n_sched_changed_locked(sched);
	if (sequence)
		*sequence = atomic64_read(&sched->sequence);
	spin_unlock_irqrestore(&sched->lock, flags);
	q3n_sched_wake(sched);
	return ret;
}

int q3n_sched_try_start(struct q3n_sched *sched, struct q3n_request *req)
{
	return q3n_sched_try_start_seq(sched, req, NULL);
}

void q3n_sched_wait_for_change(struct q3n_sched *sched, u64 sequence)
{
	wait_event(sched->waitq,
		   atomic64_read(&sched->sequence) != sequence);
}

void q3n_sched_notify(struct q3n_sched *sched)
{
	unsigned long flags;

	spin_lock_irqsave(&sched->lock, flags);
	q3n_sched_changed_locked(sched);
	spin_unlock_irqrestore(&sched->lock, flags);
	q3n_sched_wake(sched);
}

struct q3n_request *q3n_sched_pick_next(struct q3n_sched *sched)
{
	struct q3n_request *req = NULL;
	unsigned long flags;

	if (!sched)
		return NULL;

	spin_lock_irqsave(&sched->lock, flags);
	req = list_first_entry_or_null(&sched->foreground_queue,
					struct q3n_request, node);
	if (!req)
		req = list_first_entry_or_null(&sched->parity_read_queue,
						struct q3n_request, node);
	if (!req)
		req = list_first_entry_or_null(&sched->parity_write_queue,
						struct q3n_request, node);
	if (req) {
		list_del_init(&req->node);
		if (req->class != Q3N_REQ_FOREGROUND)
			sched->pending_parity--;
		q3n_sched_changed_locked(sched);
	}
	spin_unlock_irqrestore(&sched->lock, flags);
	if (req)
		q3n_sched_wake(sched);
	return req;
}

void q3n_sched_drain(struct q3n_sched *sched)
{
	struct list_head *queues[] = {
		&sched->foreground_queue,
		&sched->parity_read_queue,
		&sched->parity_write_queue,
	};
	struct q3n_request *req, *tmp;
	unsigned long flags;
	u8 i;

	spin_lock_irqsave(&sched->lock, flags);
	for (i = 0; i < ARRAY_SIZE(queues); i++)
		list_for_each_entry_safe(req, tmp, queues[i], node)
			list_del_init(&req->node);
	sched->pending_parity = 0;
	q3n_sched_changed_locked(sched);
	spin_unlock_irqrestore(&sched->lock, flags);
	q3n_sched_wake(sched);
}

MODULE_DESCRIPTION("QEMU 3D NAND foreground-priority request scheduler");
MODULE_LICENSE("GPL");
