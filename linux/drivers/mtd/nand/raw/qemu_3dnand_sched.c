// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/module.h>

#include "qemu_3dnand_priv.h"

static bool q3n_req_ready(const struct q3n_request *req)
{
	if (req->op == Q3N_REQ_READ)
		return true;
	if (!req->block_state)
		return false;

	return q3n_program_order_ready(req->block_state, req->page);
}

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

static struct q3n_request *q3n_sched_pick_ready(struct list_head *queue)
{
	struct q3n_request *req;

	list_for_each_entry(req, queue, node) {
		if (q3n_req_ready(req))
			return req;
	}

	return NULL;
}

static bool q3n_queue_has_frontier_dependency(struct list_head *queue,
					      const struct q3n_block_state *state,
					      u32 page)
{
	struct q3n_request *req;

	list_for_each_entry(req, queue, node) {
		if (req->block_state == state && req->page == page &&
		    (req->op == Q3N_REQ_PROGRAM ||
		     req->class == Q3N_REQ_PARITY_READ))
			return true;
	}
	return false;
}

static bool q3n_sched_has_frontier_dependency(struct q3n_sched *sched,
					       const struct q3n_request *req)
{
	u32 page = req->block_state->next_prog_page;

	return q3n_queue_has_frontier_dependency(&sched->foreground_queue,
						 req->block_state, page) ||
		q3n_queue_has_frontier_dependency(&sched->parity_read_queue,
						  req->block_state, page) ||
		q3n_queue_has_frontier_dependency(&sched->parity_write_queue,
						  req->block_state, page);
}

static void q3n_sched_remove_locked(struct q3n_sched *sched,
				    struct q3n_request *req)
{
	list_del_init(&req->node);
	if (req->class != Q3N_REQ_FOREGROUND)
		sched->pending_parity--;
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

void q3n_block_cancel_begin(struct q3n_block_barrier *barrier)
{
	WRITE_ONCE(barrier->cancelling, true);
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
	INIT_LIST_HEAD(&sched->foreground_queue);
	INIT_LIST_HEAD(&sched->parity_read_queue);
	INIT_LIST_HEAD(&sched->parity_write_queue);
	sched->pending_parity = 0;
	sched->reserved_parity = 0;
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
	if (req->class != Q3N_REQ_FOREGROUND)
		sched->pending_parity++;
	spin_unlock_irqrestore(&sched->lock, flags);
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
	spin_unlock_irqrestore(&sched->lock, flags);
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

int q3n_sched_try_start(struct q3n_sched *sched, struct q3n_request *req)
{
	struct q3n_request *next = NULL;
	unsigned long flags;

	if (!sched || !req)
		return -EINVAL;

	spin_lock_irqsave(&sched->lock, flags);
	if (req->op == Q3N_REQ_PROGRAM) {
		if (!req->block_state) {
			q3n_sched_remove_locked(sched, req);
			spin_unlock_irqrestore(&sched->lock, flags);
			return -EINVAL;
		}
		if (req->page < req->block_state->next_prog_page) {
			q3n_sched_remove_locked(sched, req);
			spin_unlock_irqrestore(&sched->lock, flags);
			return -ESTALE;
		}
		if (req->page > req->block_state->next_prog_page &&
		    !q3n_sched_has_frontier_dependency(sched, req)) {
			q3n_sched_remove_locked(sched, req);
			spin_unlock_irqrestore(&sched->lock, flags);
			return -ERANGE;
		}
	}
	next = q3n_sched_pick_ready(&sched->foreground_queue);
	if (!next)
		next = q3n_sched_pick_ready(&sched->parity_read_queue);
	if (!next)
		next = q3n_sched_pick_ready(&sched->parity_write_queue);
	if (next != req) {
		spin_unlock_irqrestore(&sched->lock, flags);
		return -EAGAIN;
	}

	q3n_sched_remove_locked(sched, req);
	spin_unlock_irqrestore(&sched->lock, flags);
	return 0;
}

struct q3n_request *q3n_sched_pick_next(struct q3n_sched *sched)
{
	struct q3n_request *req = NULL;
	unsigned long flags;

	if (!sched)
		return NULL;

	spin_lock_irqsave(&sched->lock, flags);
	req = q3n_sched_pick_ready(&sched->foreground_queue);
	if (!req)
		req = q3n_sched_pick_ready(&sched->parity_read_queue);
	if (!req)
		req = q3n_sched_pick_ready(&sched->parity_write_queue);
	if (req) {
		list_del_init(&req->node);
		if (req->class != Q3N_REQ_FOREGROUND)
			sched->pending_parity--;
	}
	spin_unlock_irqrestore(&sched->lock, flags);
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
	spin_unlock_irqrestore(&sched->lock, flags);
}

MODULE_DESCRIPTION("QEMU 3D NAND foreground-priority request scheduler");
MODULE_LICENSE("GPL");
