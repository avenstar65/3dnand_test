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

int q3n_profile_group(struct qemu_3dnand *q3n, loff_t addr,
				     struct q3n_raid_group *group,
				     u64 *leb_out, u8 *data_slot,
				     u32 *column)
{
	u64 leb = div64_u64(addr, q3n->mtd->erasesize);
	u64 in_leb = addr % q3n->mtd->erasesize;
	u32 page;

	if (leb >= q3n->profile_leb_count)
		return -ERANGE;
	if (q3n->raid_level == Q3N_RAID1) {
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

void q3n_profile_buffers(struct qemu_3dnand *q3n,
					const struct q3n_raid_group *group,
					struct q3n_mp_buffers *buffers,
					bool oob)
{
	u8 plane;

	memset(buffers, 0, sizeof(*buffers));
	buffers->mask = group->member_mask;
	buffers->die = group->die;
	for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++) {
		buffers->addr[plane] = group->member[plane];
		buffers->data[plane] = oob ? q3n->mp_oob[plane] :
			q3n->mp_buf[plane];
	}
}

int q3n_profile_write_page_locked(struct qemu_3dnand *q3n,
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

	ret = q3n_profile_group(q3n, to, &group, &leb, NULL, &column);
	if (ret || column)
		return ret ?: -EINVAL;
	state_index = leb * q3n->pages_per_block + group.page;
	q3n_recovery_begin(&q3n->profile_state[state_index]);
	q3n_profile_buffers(q3n, &group, &buffers, false);
	if (q3n->raid_level == Q3N_RAID1) {
		for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++)
			if (group.member_mask & BIT(plane))
				buffers.data[plane] = (u8 *)data;
	} else {
		u8 slot = 0;

		memset(q3n->raid_buf, 0, q3n->page_size);
		for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++) {
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

#if Q3N_ENABLE_MULTIPLANE_RAID
void q3n_profile_invalidate_leb(struct qemu_3dnand *q3n,
					       u64 leb)
{
	memset(&q3n->profile_state[leb * q3n->pages_per_block], 0,
	       q3n->pages_per_block * sizeof(*q3n->profile_state));
}
#endif

static int q3n_profile_read_failed(struct qemu_3dnand *q3n)
{
	q3n->mtd->ecc_stats.failed++;
	q3n->raid_failed++;
	return q3n->mtd->ecc_strength;
}

static int q3n_profile_read_raid1(struct qemu_3dnand *q3n,
		const struct q3n_raid_group *group,
		const struct q3n_mp_result *result, u64 state_index, u8 *out)
{
	u8 low = __ffs(group->member_mask);
	u8 high = fls(group->member_mask) - 1;
	u8 first = (group->stripe_id & 1) ? high : low;
	u8 second = first == low ? high : low;
	bool first_ok = (result->success_mask & BIT(first)) &&
		!result->ecc[first].uncorrectable;
	bool second_ok = (result->success_mask & BIT(second)) &&
		!result->ecc[second].uncorrectable;

	if (first_ok && second_ok && memcmp(q3n->mp_buf[first],
		q3n->mp_buf[second], q3n->page_size))
		return q3n_profile_read_failed(q3n);
	if (first_ok) {
		memcpy(out, q3n->mp_buf[first], q3n->page_size);
		qemu_3dnand_profile_account_ecc(q3n, &result->ecc[first]);
		return result->ecc[first].max_bitflips;
	}
	if (!second_ok ||
	    !q3n_recovery_allowed(q3n->profile_state[state_index]))
		return q3n_profile_read_failed(q3n);
	memcpy(out, q3n->mp_buf[second], q3n->page_size);
	qemu_3dnand_profile_account_ecc(q3n, &result->ecc[second]);
	q3n->raid_recovered++;
	return max_t(u32, result->ecc[second].max_bitflips,
		     q3n->mtd->bitflip_threshold);
}

static u8 q3n_profile_collect_raid5(struct qemu_3dnand *q3n,
		const struct q3n_raid_group *group,
		const struct q3n_mp_result *result, u8 *out, u8 *missing,
		u32 *max_bitflips)
{
	u8 failed = 0;
	u8 slot = 0;
	u8 plane;

	for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++) {
		bool ok = (result->success_mask & BIT(plane)) &&
			!result->ecc[plane].uncorrectable;

		if (plane == group->parity_plane)
			continue;
		if (!ok) {
			failed++;
			*missing = plane;
		} else {
			memcpy(out + slot * q3n->page_size, q3n->mp_buf[plane],
			       q3n->page_size);
			qemu_3dnand_profile_account_ecc(q3n, &result->ecc[plane]);
			*max_bitflips = max(*max_bitflips,
					result->ecc[plane].max_bitflips);
		}
		slot++;
	}
	return failed;
}

static int q3n_profile_recover_raid5(struct qemu_3dnand *q3n,
		const struct q3n_raid_group *group,
		const struct q3n_mp_result *result, u64 state_index, u8 missing,
		u32 max_bitflips, u8 *out)
{
	u8 missing_slot = missing < group->parity_plane ? missing : missing - 1;
	u8 source[2];
	u8 count = 0;
	u8 plane;

	if (!(result->success_mask & BIT(group->parity_plane)) ||
	    result->ecc[group->parity_plane].uncorrectable ||
	    !q3n_recovery_allowed(q3n->profile_state[state_index]))
		return q3n_profile_read_failed(q3n);
	for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++)
		if (plane != missing && plane != group->parity_plane)
			source[count++] = plane;
	q3n_raid5_recover(out + missing_slot * q3n->page_size,
		q3n->mp_buf[group->parity_plane], q3n->mp_buf[source[0]],
		q3n->mp_buf[source[1]], q3n->page_size);
	q3n->raid_recovered++;
	return max_t(u32, max_bitflips, q3n->mtd->bitflip_threshold);
}

int q3n_profile_read_page_locked(struct qemu_3dnand *q3n,
					 loff_t from, u8 *out)
{
	struct q3n_mp_buffers buffers;
	struct q3n_mp_result result;
	struct q3n_raid_group group;
	u64 leb;
	u64 state_index;
	u32 column;
	u32 max_bitflips = 0;
	u8 missing = q3n->profile_geometry.planes_per_die;
	u8 failed;
	int ret;

	ret = q3n_profile_group(q3n, from, &group, &leb, NULL,
					&column);
	if (ret)
		return ret;
	if (column)
		return -EINVAL;
	state_index = leb * q3n->pages_per_block + group.page;

	q3n_profile_buffers(q3n, &group, &buffers, false);
	ret = q3n_mp_read(&q3n->mp, &buffers, &result);
	if (ret && (ret != -EIO || !result.failure_mask ||
		    !result.success_mask))
		return ret;
	if (q3n->raid_level == Q3N_RAID1)
		return q3n_profile_read_raid1(q3n, &group, &result,
					      state_index, out);
	failed = q3n_profile_collect_raid5(q3n, &group, &result, out,
					   &missing, &max_bitflips);
	if (!failed)
		return max_bitflips;
	if (failed != 1)
		return q3n_profile_read_failed(q3n);
	return q3n_profile_recover_raid5(q3n, &group, &result, state_index,
					 missing, max_bitflips, out);
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

		ret = q3n_profile_read_page_locked(q3n,
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
		ret = q3n_profile_write_page_locked(q3n, to + done, buf + done);
		if (ret)
			break;
		done += mtd->writesize;
	}
	mutex_unlock(&q3n->mtd_lock);
	*retlen = done;
	return ret;
}

int __maybe_unused q3n_profile_erase_page_locked(struct mtd_info *mtd,
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

		ret = q3n_profile_group(q3n, instr->addr + done,
						&group, &leb, NULL, &column);
		if (!ret) {
			q3n_profile_buffers(q3n, &group, &buffers, false);
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

void __maybe_unused q3n_profile_sync(struct mtd_info *mtd)
{
	(void)mtd;
}

int __maybe_unused q3n_profile_block_bad(struct mtd_info *mtd,
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
	ret = q3n_profile_group(q3n, ofs, &group, NULL, NULL, &column);
	if (ret)
		goto out;
	q3n_profile_buffers(q3n, &group, &buffers, true);
	q3n_mp_read_oob(&q3n->mp, &buffers, &result);
	for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++) {
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

int __maybe_unused q3n_profile_block_markbad(struct mtd_info *mtd,
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
	ret = q3n_profile_group(q3n, ofs, &group, NULL, NULL, &column);
	if (ret)
		goto out;
	q3n_profile_buffers(q3n, &group, &buffers, true);
	q3n_mp_read_oob(&q3n->mp, &buffers, &result);
	for (plane = 0; plane < q3n->profile_geometry.planes_per_die; plane++) {
		if (!(group.member_mask & BIT(plane)))
			continue;
		if (!(result.success_mask & BIT(plane)))
			memset(q3n->mp_oob[plane], 0xff, q3n->oob_size);
		q3n->mp_oob[plane][0] = 0x00;
	}
	ret = q3n_mp_program_oob(&q3n->mp, &buffers, &result);
	if (result.success_mask != group.member_mask)
		ret = -EIO;
out:
	mutex_unlock(&q3n->mtd_lock);
	return ret;
}
