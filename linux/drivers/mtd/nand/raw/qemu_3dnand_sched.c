// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/module.h>

#include "qemu_3dnand_priv.h"

static bool q3n_req_ready(const struct q3n_request *req)
{
	if (req->op == Q3N_REQ_READ)
		return true;

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

void q3n_sched_init(struct q3n_sched *sched)
{
	spin_lock_init(&sched->lock);
	INIT_LIST_HEAD(&sched->foreground_queue);
	INIT_LIST_HEAD(&sched->parity_read_queue);
	INIT_LIST_HEAD(&sched->parity_write_queue);
	sched->pending_parity = 0;
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
	if (req->class != Q3N_REQ_FOREGROUND &&
	    sched->pending_parity >= Q3N_MAX_PENDING_PARITY) {
		spin_unlock_irqrestore(&sched->lock, flags);
		return -ENOSPC;
	}
	INIT_LIST_HEAD(&req->node);
	list_add_tail(&req->node, queue);
	if (req->class != Q3N_REQ_FOREGROUND)
		sched->pending_parity++;
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
