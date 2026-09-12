// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/unaligned.h>

#include "qemu_3dnand.h"
#include "qemu_3dnand_priv.h"

static const struct q3n_geometry q3n_test_geometry = {
	.page_size = 16 * 1024,
	.pages_per_block = 1600,
	.data_pages_per_stripe = 7,
	.data_block_count = 14,
	.parity_block_count = 2,
};

static struct q3n_geometry q3n_test_raid_geometry(enum q3n_raid_level level)
{
	return (struct q3n_geometry) {
		.page_size = 16 * 1024,
		.pages_per_block = 1600,
		.blocks_per_plane = 247,
		.data_blocks_per_plane = 208,
		.dies = 2,
		.planes_per_die = 4,
		.raid_level = level,
	};
}

static void q3n_raid1_plane_pair_mapping_test(struct kunit *test)
{
	struct q3n_geometry geometry = q3n_test_raid_geometry(Q3N_RAID1);
	struct q3n_raid_group group;
	static const u8 dies[] = { 0, 0, 1, 1, 0 };
	static const u8 masks[] = { 0x03, 0x0c, 0x03, 0x0c, 0x03 };
	u64 leb;

	for (leb = 0; leb < ARRAY_SIZE(dies); leb++) {
		KUNIT_ASSERT_EQ(test, q3n_map_raid1_page(&geometry, leb, 7,
						       &group), 0);
		KUNIT_EXPECT_EQ(test, group.die, dies[leb]);
		KUNIT_EXPECT_EQ(test, group.member_mask, masks[leb]);
		KUNIT_EXPECT_EQ(test, group.block_in_plane,
				(u32)(leb / 4));
	}
}

static void q3n_raid5_die_block_and_parity_rotation_test(struct kunit *test)
{
	struct q3n_geometry geometry = q3n_test_raid_geometry(Q3N_RAID5);
	struct q3n_raid_group group;
	static const u8 dies[] = { 0, 1, 0 };
	static const u32 blocks[] = { 0, 0, 1 };
	u64 leb;
	u32 page;

	for (leb = 0; leb < ARRAY_SIZE(dies); leb++) {
		KUNIT_ASSERT_EQ(test, q3n_map_raid5_stripe(&geometry, leb, 0,
							  &group), 0);
		KUNIT_EXPECT_EQ(test, group.die, dies[leb]);
		KUNIT_EXPECT_EQ(test, group.block_in_plane, blocks[leb]);
		KUNIT_EXPECT_EQ(test, group.member_mask, (u8)0x0f);
	}
	for (page = 0; page < 4; page++) {
		KUNIT_ASSERT_EQ(test, q3n_map_raid5_stripe(&geometry, 0, page,
							  &group), 0);
		KUNIT_EXPECT_EQ(test, group.parity_plane, (u8)page);
	}
}

static void q3n_profile_geometry_values_test(struct kunit *test)
{
	struct q3n_geometry geometry = q3n_test_raid_geometry(Q3N_RAID1);
	u32 writesize;
	u32 erasesize;
	u64 size;

	KUNIT_ASSERT_EQ(test, q3n_raid_geometry_values(&geometry, &writesize,
						       &erasesize, &size), 0);
	KUNIT_EXPECT_EQ(test, writesize, 16384U);
	KUNIT_EXPECT_EQ(test, erasesize, 26214400U);
	KUNIT_EXPECT_EQ(test, size, 21810380800ULL);

	geometry.raid_level = Q3N_RAID5;
	KUNIT_ASSERT_EQ(test, q3n_raid_geometry_values(&geometry, &writesize,
						       &erasesize, &size), 0);
	KUNIT_EXPECT_EQ(test, writesize, 49152U);
	KUNIT_EXPECT_EQ(test, erasesize, 78643200U);
	KUNIT_EXPECT_EQ(test, size, 32715571200ULL);
}

static void q3n_raid5_xor_recovery_test(struct kunit *test)
{
	struct q3n_geometry geometry = q3n_test_raid_geometry(Q3N_RAID5);
	struct q3n_raid_group group;
	u8 d0[16] = { 0x11 };
	u8 d1[16] = { 0x22 };
	u8 d2[16] = { 0x33 };
	u8 parity[16] = {};
	u8 recovered[16];

	KUNIT_ASSERT_EQ(test, q3n_map_raid5_stripe(&geometry, 0, 3, &group), 0);
	KUNIT_EXPECT_EQ(test, group.member_mask, (u8)0x0f);
	KUNIT_EXPECT_EQ(test, group.parity_plane, (u8)3);

	q3n_xor_page(parity, d0, sizeof(parity));
	q3n_xor_page(parity, d1, sizeof(parity));
	q3n_xor_page(parity, d2, sizeof(parity));
	KUNIT_ASSERT_EQ(test, q3n_raid5_recover(recovered, parity, d0, d1,
						 sizeof(recovered)), 0);
	KUNIT_EXPECT_MEMEQ(test, recovered, d2, sizeof(recovered));
	KUNIT_EXPECT_EQ(test, q3n_raid5_recover(recovered, parity, NULL, d1,
						 sizeof(recovered)), -EINVAL);
}

static void q3n_build_bad_block_oob(u8 *logical_oob)
{
	memset(logical_oob, 0xff, Q3N_LOGICAL_OOB_SIZE);
	logical_oob[0] = 0x00;
}

static void q3n_bad_block_oob_marks_only_bbm_test(struct kunit *test)
{
	u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];
	u32 i;

	q3n_build_bad_block_oob(logical_oob);
	KUNIT_EXPECT_EQ(test, logical_oob[0], (u8)0x00);
	for (i = 1; i < Q3N_LOGICAL_OOB_SIZE; i++)
		KUNIT_EXPECT_EQ(test, logical_oob[i], (u8)0xff);
}

static void q3n_map_separate_parity_block_test(struct kunit *test)
{
	struct q3n_phys_addr data;
	struct q3n_phys_addr parity;

	KUNIT_ASSERT_EQ(test, q3n_map_data_page(&q3n_test_geometry, 0, 6,
						&data), 0);
	KUNIT_ASSERT_EQ(test, q3n_map_parity_page(&q3n_test_geometry, 0,
						  &parity), 0);
	KUNIT_EXPECT_NE(test, data.block, parity.block);
	KUNIT_EXPECT_EQ(test, data.block, 6U);
	KUNIT_EXPECT_EQ(test, data.page, 0U);
	KUNIT_EXPECT_EQ(test, parity.block, 14U);
	KUNIT_EXPECT_EQ(test, parity.page, 0U);
}

static void q3n_map_non_power_of_two_geometry_test(struct kunit *test)
{
	struct q3n_phys_addr data;
	struct q3n_phys_addr parity;

	KUNIT_ASSERT_EQ(test, q3n_map_data_page(&q3n_test_geometry, 1600, 2,
						&data), 0);
	KUNIT_ASSERT_EQ(test, q3n_map_parity_page(&q3n_test_geometry, 1600,
						  &parity), 0);
	KUNIT_EXPECT_EQ(test, data.block, 9U);
	KUNIT_EXPECT_EQ(test, data.page, 0U);
	KUNIT_EXPECT_EQ(test, parity.block, 15U);
	KUNIT_EXPECT_EQ(test, parity.page, 0U);
}

static void q3n_map_rejects_invalid_input_test(struct kunit *test)
{
	struct q3n_phys_addr out;
	struct q3n_geometry geometry = q3n_test_geometry;

	KUNIT_EXPECT_EQ(test, q3n_map_data_page(&geometry, 0, 7, &out),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, q3n_map_parity_page(&geometry, 3200, &out),
			-ERANGE);
	geometry.pages_per_block = 0;
	KUNIT_EXPECT_EQ(test, q3n_map_data_page(&geometry, 0, 0, &out),
			-EINVAL);
}

static void q3n_scheduler_does_not_gate_program_page_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_request p8 = {
		.class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_PROGRAM,
		.page = 8,
	};

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &p8), 0);
	KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &p8), 0);
}

static void q3n_next_stripe_data_preempts_previous_parity_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_request parity = {
		.class = Q3N_REQ_PARITY_WRITE,
		.op = Q3N_REQ_PROGRAM,
		.page = 7,
	};
	struct q3n_request next_data = {
		.class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_PROGRAM,
		.page = 8,
	};

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &parity), 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &next_data), 0);
	KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &next_data), 0);
	KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &parity), 0);
}

static void q3n_serial_same_block_mapping_test(struct kunit *test)
{
	struct q3n_phys_addr first, last, next;

	KUNIT_ASSERT_EQ(test, q3n_map_serial_data_page(&q3n_test_geometry, 0,
							  &first), 0);
	KUNIT_ASSERT_EQ(test, q3n_map_serial_data_page(&q3n_test_geometry, 6,
							  &last), 0);
	KUNIT_ASSERT_EQ(test, q3n_map_serial_data_page(&q3n_test_geometry, 7,
							  &next), 0);
	KUNIT_EXPECT_EQ(test, first.block, 0U);
	KUNIT_EXPECT_EQ(test, first.page, 0U);
	KUNIT_EXPECT_EQ(test, last.block, 0U);
	KUNIT_EXPECT_EQ(test, last.page, 6U);
	KUNIT_EXPECT_EQ(test, next.block, 0U);
	KUNIT_EXPECT_EQ(test, next.page, 8U);
}

static void q3n_incremental_xor_test(struct kunit *test)
{
	u8 d0[16] = { 0x55 };
	u8 d1[16] = { 0xaa };
	u8 parity[16] = {};

	q3n_xor_page(parity, d0, sizeof(parity));
	q3n_xor_page(parity, d1, sizeof(parity));
	KUNIT_EXPECT_EQ(test, parity[0], (u8)0xff);
	KUNIT_EXPECT_EQ(test, parity[1], (u8)0x00);
}

static void q3n_ecc_accumulate_clean_test(struct kunit *test)
{
	struct q3n_ecc_result total = {};
	struct q3n_ecc_result page = {};

	q3n_ecc_accumulate(&total, &page);
	KUNIT_EXPECT_EQ(test, total.max_bitflips, 0U);
	KUNIT_EXPECT_EQ(test, total.corrected_bits, 0U);
	KUNIT_EXPECT_FALSE(test, total.uncorrectable);
	KUNIT_EXPECT_EQ(test, q3n_ecc_result_to_mtd_ret(&total), 0);
}

static void
q3n_ecc_accumulate_uses_max_and_sums_corrected_test(struct kunit *test)
{
	struct q3n_ecc_result total = {};
	struct q3n_ecc_result first = {
		.max_bitflips = 30,
		.corrected_bits = 30,
	};
	struct q3n_ecc_result second = {
		.max_bitflips = 20,
		.corrected_bits = 20,
	};

	q3n_ecc_accumulate(&total, &first);
	q3n_ecc_accumulate(&total, &second);
	KUNIT_EXPECT_EQ(test, total.max_bitflips, 30U);
	KUNIT_EXPECT_EQ(test, total.corrected_bits, 50U);
	KUNIT_EXPECT_EQ(test, q3n_ecc_result_to_mtd_ret(&total), 30);
}

static void q3n_ecc_accumulate_preserves_threshold_test(struct kunit *test)
{
	struct q3n_ecc_result total = {};
	struct q3n_ecc_result page = {
		.max_bitflips = 40,
		.corrected_bits = 40,
	};

	q3n_ecc_accumulate(&total, &page);
	KUNIT_EXPECT_EQ(test, total.max_bitflips, 40U);
	KUNIT_EXPECT_EQ(test, q3n_ecc_result_to_mtd_ret(&total), 40);
}

static void
q3n_ecc_accumulate_defers_failure_accounting_once_test(struct kunit *test)
{
	struct q3n_ecc_result total = {};
	struct q3n_ecc_result first = {
		.failed_step = 3,
		.uncorrectable = true,
	};
	struct q3n_ecc_result second = {
		.failed_step = 9,
		.uncorrectable = true,
	};
	u32 failed = 0;

	q3n_ecc_accumulate(&total, &first);
	q3n_ecc_accumulate(&total, &second);
	if (total.uncorrectable)
		failed++;

	KUNIT_EXPECT_TRUE(test, total.uncorrectable);
	KUNIT_EXPECT_EQ(test, total.failed_step, 3U);
	KUNIT_EXPECT_EQ(test, failed, 1U);
	KUNIT_EXPECT_EQ(test, total.max_bitflips, 0U);
	KUNIT_EXPECT_EQ(test, q3n_ecc_result_to_mtd_ret(&total), -EBADMSG);
	KUNIT_EXPECT_EQ(test, failed, 1U);
}

static void q3n_open_stripe_is_unprotected_until_parity_completes_test(struct kunit *test)
{
	u8 first_parity[16] = {};
	u8 second_parity[16] = {};
	u8 data[16] = { 0x1 };
	struct q3n_open_stripe first = {
		.stripe_id = 99,
		.data_pages = Q3N_RAID_MAX_DATA_PAGES,
		.parity = first_parity,
		.page_size = sizeof(first_parity),
	};
	struct q3n_open_stripe second = {
		.stripe_id = 100,
		.data_pages = Q3N_RAID_MAX_DATA_PAGES,
		.parity = second_parity,
		.page_size = sizeof(second_parity),
	};
	u8 slot;

	for (slot = 0; slot < Q3N_RAID_MAX_DATA_PAGES; slot++)
		KUNIT_ASSERT_EQ(test, q3n_open_stripe_update(&first, slot, data,
							     sizeof(data)), 0);
	KUNIT_ASSERT_EQ(test, q3n_open_stripe_queue_parity(&first), 0);
	KUNIT_EXPECT_EQ(test, first.state, Q3N_STRIPE_PARITY_QUEUED);
	q3n_open_stripe_complete_parity(&first, false);
	KUNIT_EXPECT_EQ(test, first.state, Q3N_STRIPE_UNPROTECTED);

	for (slot = 0; slot < Q3N_RAID_MAX_DATA_PAGES; slot++)
		KUNIT_ASSERT_EQ(test, q3n_open_stripe_update(&second, slot, data,
							     sizeof(data)), 0);
	KUNIT_EXPECT_EQ(test, q3n_open_stripe_queue_parity(&second), 0);
	KUNIT_EXPECT_EQ(test, second.state, Q3N_STRIPE_PARITY_QUEUED);
	q3n_open_stripe_complete_parity(&second, true);
	KUNIT_EXPECT_EQ(test, second.state, Q3N_STRIPE_PROTECTED);
}

static void q3n_recover_single_missing_page_test(struct kunit *test)
{
	u8 d0[4] = { 0x10, 0x20, 0x30, 0x40 };
	u8 d1[4] = { 0x01, 0x02, 0x03, 0x04 };
	u8 parity[4] = {};
	u8 recovered[4] = {};
	const u8 *members[] = { d0, NULL };

	q3n_xor_page(parity, d0, sizeof(parity));
	q3n_xor_page(parity, d1, sizeof(parity));
	KUNIT_ASSERT_EQ(test, q3n_recover_page(recovered, parity, members, 2,
					       1, sizeof(recovered)), 0);
	KUNIT_EXPECT_MEMEQ(test, recovered, d1, sizeof(recovered));
}

static void q3n_rebuild_processes_one_member_per_step_test(struct kunit *test)
{
	u8 parity[4] = { 0x11, 0x22, 0x33, 0x44 };
	u8 member[4] = { 0x01, 0x02, 0x03, 0x04 };
	struct q3n_parity_rebuild rebuild = {
		.data_pages = 7,
		.missing_slot = 3,
		.parity_accumulator = parity,
		.page_size = sizeof(parity),
	};

	KUNIT_ASSERT_EQ(test, q3n_rebuild_xor_one(&rebuild, member), 0);
	KUNIT_EXPECT_EQ(test, rebuild.next_slot, (u8)1);
	KUNIT_EXPECT_EQ(test, parity[0], (u8)0x10);
}

static void q3n_rebuild_rejects_stale_generation_test(struct kunit *test)
{
	struct q3n_parity_rebuild rebuild = { .generation = 1 };

	KUNIT_EXPECT_EQ(test, q3n_rebuild_check_generation(&rebuild, 1), 0);
	KUNIT_EXPECT_EQ(test, q3n_rebuild_check_generation(&rebuild, 2),
			-ESTALE);
}

static void q3n_scheduler_prioritizes_ready_foreground_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_request foreground = { .class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_READ };
	struct q3n_request parity = { .class = Q3N_REQ_PARITY_WRITE,
		.op = Q3N_REQ_PROGRAM };

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &parity), 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &foreground), 0);
	KUNIT_EXPECT_PTR_EQ(test, q3n_sched_pick_next(&sched), &foreground);
	KUNIT_EXPECT_PTR_EQ(test, q3n_sched_pick_next(&sched), &parity);
}

static void q3n_scheduler_prioritizes_parity_read_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_request read = { .class = Q3N_REQ_PARITY_READ,
		.op = Q3N_REQ_READ };
	struct q3n_request write = { .class = Q3N_REQ_PARITY_WRITE,
		.op = Q3N_REQ_READ };

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &write), 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &read), 0);
	KUNIT_EXPECT_PTR_EQ(test, q3n_sched_pick_next(&sched), &read);
}

static void q3n_scheduler_foreground_preempts_requeued_rebuild_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_request rebuild = { .class = Q3N_REQ_PARITY_READ,
		.op = Q3N_REQ_READ };
	struct q3n_request foreground = { .class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_READ };

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &rebuild), 0);
	KUNIT_ASSERT_PTR_EQ(test, q3n_sched_pick_next(&sched), &rebuild);
	KUNIT_ASSERT_EQ(test, q3n_sched_requeue_p1(&sched, &rebuild), 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &foreground), 0);
	KUNIT_EXPECT_PTR_EQ(test, q3n_sched_pick_next(&sched), &foreground);
}

static void q3n_scheduler_p1_claim_preserves_foreground_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_request rebuild = { .class = Q3N_REQ_PARITY_READ,
		.op = Q3N_REQ_READ };
	struct q3n_request foreground = { .class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_READ };

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &rebuild), 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &foreground), 0);
	KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &rebuild), -EAGAIN);
	KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &foreground), 0);
	KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &rebuild), 0);
}

static void q3n_scheduler_reserves_capacity_per_stripe_test(struct kunit *test)
{
	struct q3n_sched sched;
	u32 i;

	q3n_sched_init(&sched);
	for (i = 0; i < Q3N_MAX_PENDING_PARITY; i++)
		KUNIT_ASSERT_EQ(test, q3n_sched_reserve_parity(&sched), 0);
	KUNIT_EXPECT_EQ(test, q3n_sched_reserve_parity(&sched), -ENOSPC);
	q3n_sched_release_parity(&sched);
	KUNIT_EXPECT_EQ(test, q3n_sched_reserve_parity(&sched), 0);
}

static void q3n_scheduler_tracks_max_pending_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_request first = { .class = Q3N_REQ_PARITY_READ,
		.op = Q3N_REQ_READ };
	struct q3n_request second = { .class = Q3N_REQ_PARITY_WRITE,
		.op = Q3N_REQ_PROGRAM };
	u32 pending;
	u32 reserved;
	u32 max_pending;

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &first), 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &second), 0);
	KUNIT_ASSERT_PTR_EQ(test, q3n_sched_pick_next(&sched), &first);
	q3n_sched_get_counts(&sched, &pending, &reserved, &max_pending);
	KUNIT_EXPECT_EQ(test, pending, 1U);
	KUNIT_EXPECT_EQ(test, max_pending, 2U);
}

struct q3n_scheduler_wait_test_ctx {
	struct q3n_sched *sched;
	struct q3n_request *request;
	struct completion first_attempt;
	struct completion done;
	atomic_t attempts;
	int result;
};

static int q3n_scheduler_wait_test_thread(void *data)
{
	struct q3n_scheduler_wait_test_ctx *ctx = data;
	u64 sequence;

	atomic_inc(&ctx->attempts);
	ctx->result = q3n_sched_try_start_seq(ctx->sched, ctx->request,
						  &sequence);
	complete(&ctx->first_attempt);
	if (ctx->result == -EAGAIN) {
		q3n_sched_wait_for_change(ctx->sched, sequence);
		atomic_inc(&ctx->attempts);
		ctx->result = q3n_sched_try_start_seq(ctx->sched, ctx->request,
							  &sequence);
	}
	complete(&ctx->done);
	return 0;
}

static void q3n_scheduler_waits_for_state_change_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_request foreground = {
		.class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_READ,
	};
	struct q3n_request parity = {
		.class = Q3N_REQ_PARITY_READ,
		.op = Q3N_REQ_READ,
	};
	struct q3n_scheduler_wait_test_ctx ctx = {
		.sched = &sched,
		.request = &parity,
	};
	struct task_struct *task;

	q3n_sched_init(&sched);
	init_completion(&ctx.first_attempt);
	init_completion(&ctx.done);
	atomic_set(&ctx.attempts, 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &foreground), 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &parity), 0);
	task = kthread_run(q3n_scheduler_wait_test_thread, &ctx,
			   "q3n-sched-wait-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	KUNIT_ASSERT_NE(test,
		wait_for_completion_timeout(&ctx.first_attempt,
					    msecs_to_jiffies(1000)), 0UL);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx.attempts), 1);
	KUNIT_EXPECT_FALSE(test, completion_done(&ctx.done));

	KUNIT_ASSERT_EQ(test, q3n_sched_cancel(&sched, &foreground), 0);
	KUNIT_ASSERT_NE(test,
		wait_for_completion_timeout(&ctx.done, msecs_to_jiffies(1000)),
		0UL);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx.attempts), 2);
	KUNIT_EXPECT_EQ(test, ctx.result, 0);
	kthread_stop(task);
}

static void q3n_block_barrier_blocks_new_work_and_drains_test(struct kunit *test)
{
	struct q3n_block_barrier barrier;

	q3n_block_barrier_init(&barrier);
	KUNIT_EXPECT_EQ(test, q3n_block_pending(&barrier), 0);
	KUNIT_ASSERT_EQ(test, q3n_block_parity_get(&barrier), 0);
	KUNIT_EXPECT_EQ(test, q3n_block_pending(&barrier), 1);
	KUNIT_ASSERT_EQ(test, q3n_block_cancel_begin(&barrier), 0);
	KUNIT_EXPECT_TRUE(test, q3n_block_is_cancelling(&barrier));
	KUNIT_EXPECT_EQ(test, q3n_block_cancel_begin(&barrier), -EBUSY);
	KUNIT_EXPECT_TRUE(test, q3n_block_is_cancelling(&barrier));
	KUNIT_EXPECT_EQ(test, q3n_block_parity_get(&barrier), -EBUSY);
	KUNIT_EXPECT_TRUE(test, q3n_block_parity_put(&barrier));
	KUNIT_EXPECT_EQ(test, q3n_block_pending(&barrier), 0);
	KUNIT_EXPECT_TRUE(test, q3n_block_is_cancelling(&barrier));
	q3n_block_cancel_end(&barrier);
	KUNIT_EXPECT_FALSE(test, q3n_block_is_cancelling(&barrier));
	KUNIT_EXPECT_EQ(test, q3n_block_cancel_begin(&barrier), 0);
	KUNIT_EXPECT_TRUE(test, q3n_block_is_cancelling(&barrier));
	q3n_block_cancel_end(&barrier);
}

static void q3n_block_barrier_terminal_put_wakes_on_drain_test(struct kunit *test)
{
	struct q3n_block_barrier success;
	struct q3n_block_barrier cancel;

	q3n_block_barrier_init(&success);
	KUNIT_ASSERT_EQ(test, q3n_block_parity_get(&success), 0);
	KUNIT_EXPECT_TRUE(test, q3n_block_parity_put(&success));
	KUNIT_EXPECT_EQ(test, q3n_block_pending(&success), 0);

	q3n_block_barrier_init(&cancel);
	KUNIT_ASSERT_EQ(test, q3n_block_parity_get(&cancel), 0);
	KUNIT_ASSERT_EQ(test, q3n_block_cancel_begin(&cancel), 0);
	KUNIT_EXPECT_TRUE(test, q3n_block_parity_put(&cancel));
	KUNIT_EXPECT_EQ(test, q3n_block_pending(&cancel), 0);
}

static void q3n_ram_only_recovery_qualification_test(struct kunit *test)
{
	enum q3n_stripe_state state = Q3N_STRIPE_EMPTY;

	KUNIT_EXPECT_FALSE(test, q3n_recovery_allowed(state));
	q3n_recovery_begin(&state);
	KUNIT_EXPECT_EQ(test, state, Q3N_STRIPE_UNPROTECTED);
	q3n_recovery_complete(&state, false);
	KUNIT_EXPECT_FALSE(test, q3n_recovery_allowed(state));
	q3n_recovery_begin(&state);
	q3n_recovery_complete(&state, true);
	KUNIT_EXPECT_EQ(test, state, Q3N_STRIPE_PROTECTED);
	KUNIT_EXPECT_TRUE(test, q3n_recovery_allowed(state));
	q3n_recovery_begin(&state);
	KUNIT_EXPECT_FALSE(test, q3n_recovery_allowed(state));
}

static struct kunit_case q3n_map_test_cases[] = {
	KUNIT_CASE(q3n_bad_block_oob_marks_only_bbm_test),
	KUNIT_CASE(q3n_raid1_plane_pair_mapping_test),
	KUNIT_CASE(q3n_raid5_die_block_and_parity_rotation_test),
	KUNIT_CASE(q3n_profile_geometry_values_test),
	KUNIT_CASE(q3n_raid5_xor_recovery_test),
	KUNIT_CASE(q3n_map_separate_parity_block_test),
	KUNIT_CASE(q3n_map_non_power_of_two_geometry_test),
	KUNIT_CASE(q3n_map_rejects_invalid_input_test),
	KUNIT_CASE(q3n_scheduler_does_not_gate_program_page_test),
	KUNIT_CASE(q3n_next_stripe_data_preempts_previous_parity_test),
	KUNIT_CASE(q3n_serial_same_block_mapping_test),
	KUNIT_CASE(q3n_incremental_xor_test),
	KUNIT_CASE(q3n_ecc_accumulate_clean_test),
	KUNIT_CASE(q3n_ecc_accumulate_uses_max_and_sums_corrected_test),
	KUNIT_CASE(q3n_ecc_accumulate_preserves_threshold_test),
	KUNIT_CASE(q3n_ecc_accumulate_defers_failure_accounting_once_test),
	KUNIT_CASE(q3n_open_stripe_is_unprotected_until_parity_completes_test),
	KUNIT_CASE(q3n_recover_single_missing_page_test),
	KUNIT_CASE(q3n_rebuild_processes_one_member_per_step_test),
	KUNIT_CASE(q3n_rebuild_rejects_stale_generation_test),
	KUNIT_CASE(q3n_scheduler_prioritizes_ready_foreground_test),
	KUNIT_CASE(q3n_scheduler_prioritizes_parity_read_test),
	KUNIT_CASE(q3n_scheduler_foreground_preempts_requeued_rebuild_test),
	KUNIT_CASE(q3n_scheduler_p1_claim_preserves_foreground_test),
	KUNIT_CASE(q3n_scheduler_reserves_capacity_per_stripe_test),
	KUNIT_CASE(q3n_scheduler_tracks_max_pending_test),
	KUNIT_CASE(q3n_scheduler_waits_for_state_change_test),
	KUNIT_CASE(q3n_block_barrier_blocks_new_work_and_drains_test),
	KUNIT_CASE(q3n_block_barrier_terminal_put_wakes_on_drain_test),
	KUNIT_CASE(q3n_ram_only_recovery_qualification_test),
	{}
};

static struct kunit_suite q3n_map_test_suite = {
	.name = "qemu-3dnand-map",
	.test_cases = q3n_map_test_cases,
};

kunit_test_suite(q3n_map_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for the QEMU 3D NAND mapper");
