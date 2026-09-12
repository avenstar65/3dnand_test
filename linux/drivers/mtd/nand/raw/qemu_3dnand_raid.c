// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "qemu_3dnand.h"
#include "qemu_3dnand_priv.h"

void q3n_recovery_begin(enum q3n_stripe_state *state)
{
	if (state)
		*state = Q3N_STRIPE_UNPROTECTED;
}

void q3n_recovery_complete(enum q3n_stripe_state *state, bool success)
{
	if (state)
		*state = success ? Q3N_STRIPE_PROTECTED : Q3N_STRIPE_UNPROTECTED;
}

bool q3n_recovery_allowed(enum q3n_stripe_state state)
{
	return state == Q3N_STRIPE_PROTECTED;
}

static u16 q3n_member_mask(u8 data_pages)
{
	return GENMASK(data_pages - 1, 0);
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

int q3n_raid5_recover(u8 *out, const u8 *parity, const u8 *other0,
		      const u8 *other1, size_t len)
{
	if (!out || !parity || !other0 || !other1 || !len)
		return -EINVAL;
	memcpy(out, parity, len);
	q3n_xor_page(out, other0, len);
	q3n_xor_page(out, other1, len);
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

MODULE_DESCRIPTION("QEMU 3D NAND page-RAID XOR and recovery state");
MODULE_LICENSE("GPL");
