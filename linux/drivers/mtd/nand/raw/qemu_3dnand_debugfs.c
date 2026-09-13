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

static int qemu_3dnand_inject_data_loss(void *data, u64 value)
{
	struct qemu_3dnand *q3n = data;
	int ret;

	mutex_lock(&q3n->mtd_lock);
	q3n_hw_writel(q3n, Q3N_REG_FAULT_ADDR_LO, lower_32_bits(value));
	q3n_hw_writel(q3n, Q3N_REG_FAULT_ADDR_HI, upper_32_bits(value));
	q3n_hw_writel(q3n, Q3N_REG_FAULT_CTRL,
			   Q3N_FAULT_INJECT_DATA_LOSS);
	ret = q3n_hw_wait_ready_locked(q3n);
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
	q3n_hw_writel(q3n, Q3N_REG_FAULT_ADDR_LO, lower_32_bits(addr));
	q3n_hw_writel(q3n, Q3N_REG_FAULT_ADDR_HI, upper_32_bits(addr));
	q3n_hw_writel(q3n, Q3N_REG_FAULT_CTRL,
			   Q3N_FAULT_INJECT_DATA_LOSS);
	ret = q3n_hw_wait_ready_locked(q3n);
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
	phys_addr = q3n_hw_phys_addr(q3n, block, parity_page);

	mutex_lock(&q3n->mtd_lock);
	q3n_hw_writel(q3n, Q3N_REG_FAULT_ADDR_LO,
			   lower_32_bits(phys_addr));
	q3n_hw_writel(q3n, Q3N_REG_FAULT_ADDR_HI,
			   upper_32_bits(phys_addr));
	q3n_hw_writel(q3n, Q3N_REG_FAULT_CTRL,
			   Q3N_FAULT_FAIL_NEXT_PROGRAM);
	ret = q3n_hw_wait_ready_locked(q3n);
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
	q3n_hw_writel(q3n, Q3N_REG_FAULT_CTRL, 0);
	ret = q3n_hw_wait_ready_locked(q3n);
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
	q3n_hw_writel(q3n, Q3N_REG_CMD, Q3N_CMD_RESET);
	ret = q3n_hw_wait_ready_locked(q3n);
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
	q3n_hw_writel(q3n, Q3N_REG_FAULT_ADDR_LO,
			   lower_32_bits(invalid_addr));
	q3n_hw_writel(q3n, Q3N_REG_FAULT_ADDR_HI,
			   upper_32_bits(invalid_addr));
	q3n_hw_writel(q3n, Q3N_REG_FAULT_CTRL,
			   Q3N_FAULT_FAIL_NEXT_PROGRAM);
	ret = q3n_hw_wait_ready_locked(q3n);
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
	*value = q3n_hw_readl(q3n, _reg); \
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
	struct qemu_3dnand *q3n = data;

	*value = q3n->raid_level;
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

	*value = q3n_hw_readl(q3n, Q3N_REG_STAT_FAULTS_INJECTED);
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

static void q3n_debugfs_faults(struct qemu_3dnand *q3n)
{
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
}

static void q3n_debugfs_hardware_stats(struct qemu_3dnand *q3n)
{
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
	debugfs_create_file("faults_injected", 0400, q3n->debugfs_dir, q3n,
			    &qemu_3dnand_faults_injected_fops);
}

static void q3n_debugfs_raid_stats(struct qemu_3dnand *q3n)
{
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
}

static void q3n_debugfs_pause_controls(struct qemu_3dnand *q3n)
{
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

void q3n_debugfs_init(struct qemu_3dnand *q3n)
{
	q3n->debugfs_dir = debugfs_create_dir("qemu_3dnand", NULL);
	q3n_debugfs_faults(q3n);
	q3n_debugfs_hardware_stats(q3n);
	q3n_debugfs_raid_stats(q3n);
	q3n_debugfs_pause_controls(q3n);
}

void q3n_debugfs_remove(struct qemu_3dnand *q3n)
{
	debugfs_remove_recursive(q3n->debugfs_dir);
	q3n->debugfs_dir = NULL;
}

void q3n_debugfs_unpause(struct qemu_3dnand *q3n)
{
	WRITE_ONCE(q3n->parity_pause_enable, false);
	WRITE_ONCE(q3n->parity_continuation_pause_enable, false);
	wake_up_all(&q3n->parity_pause_waitq);
}
