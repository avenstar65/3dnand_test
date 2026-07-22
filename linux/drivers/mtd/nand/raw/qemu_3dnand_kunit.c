// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/unaligned.h>

#include "qemu_3dnand_priv.h"

static const struct q3n_geometry q3n_test_geometry = {
	.page_size = 16 * 1024,
	.pages_per_block = 1600,
	.data_pages_per_stripe = 7,
	.data_block_count = 14,
	.parity_block_count = 2,
};

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

static void q3n_replay_serial_frontier_test(struct kunit *test)
{
	u8 data_valid[16] = {};
	u8 parity_valid[2] = {};
	bool needs_tail;

	KUNIT_ASSERT_EQ(test, q3n_replay_serial_frontier(16, 10,
						 data_valid, parity_valid,
						 &needs_tail), 0);
	KUNIT_EXPECT_EQ(test, data_valid[0], 1);
	KUNIT_EXPECT_EQ(test, data_valid[6], 1);
	KUNIT_EXPECT_EQ(test, data_valid[7], 0);
	KUNIT_EXPECT_EQ(test, data_valid[8], 1);
	KUNIT_EXPECT_EQ(test, data_valid[9], 1);
	KUNIT_EXPECT_EQ(test, parity_valid[0], 1);
	KUNIT_EXPECT_EQ(test, parity_valid[1], 0);
	KUNIT_EXPECT_FALSE(test, needs_tail);

	memset(data_valid, 0, sizeof(data_valid));
	memset(parity_valid, 0, sizeof(parity_valid));
	KUNIT_ASSERT_EQ(test, q3n_replay_serial_frontier(16, 7,
						 data_valid, parity_valid,
						 &needs_tail), 0);
	KUNIT_EXPECT_TRUE(test, needs_tail);
	KUNIT_EXPECT_EQ(test, q3n_replay_serial_frontier(16, 17,
						 data_valid, parity_valid,
						 &needs_tail), -EINVAL);
	KUNIT_EXPECT_EQ(test, q3n_replay_serial_frontier(15, 0,
						 data_valid, parity_valid,
						 &needs_tail), -EINVAL);
	KUNIT_EXPECT_EQ(test, q3n_replay_serial_frontier(16, 0, NULL,
						 parity_valid, &needs_tail),
			-EINVAL);
}

static void q3n_program_order_test(struct kunit *test)
{
	struct q3n_block_state state = { .next_prog_page = 17 };

	KUNIT_EXPECT_TRUE(test, q3n_program_order_ready(&state, 17));
	KUNIT_EXPECT_FALSE(test, q3n_program_order_ready(&state, 16));
	KUNIT_EXPECT_FALSE(test, q3n_program_order_ready(&state, 18));
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

static void q3n_manifest_rejects_header_crc_corruption_test(struct kunit *test)
{
	u8 parity[16] = {};
	u8 data[16] = { 0x7c };
	struct q3n_open_stripe stripe = {
		.stripe_id = 12,
		.data_pages = 1,
		.parity = parity,
		.page_size = sizeof(parity),
	};
	struct q3n_parity_manifest manifest;

	KUNIT_ASSERT_EQ(test, q3n_open_stripe_update(&stripe, 0, data,
						       sizeof(data)), 0);
	KUNIT_ASSERT_EQ(test, q3n_build_manifest(&stripe, &manifest), 0);
	KUNIT_EXPECT_EQ(test, q3n_validate_manifest(&manifest), 0);
	manifest.header_crc ^= cpu_to_le32(1);
	KUNIT_EXPECT_EQ(test, q3n_validate_manifest(&manifest), -EBADMSG);
}

static void q3n_data_oob_round_trip_preserves_bbm_test(struct kunit *test)
{
	u8 logical_oob[128] = {};
	struct q3n_data_meta meta = {
		.slot = 3,
		.stripe_id = cpu_to_le64(0x123456789ULL),
		.data_crc = cpu_to_le32(0xa5a55a5a),
	};
	struct q3n_data_meta decoded;

	KUNIT_ASSERT_EQ(test, q3n_pack_data_oob(logical_oob,
						 sizeof(logical_oob), &meta), 0);
	KUNIT_EXPECT_EQ(test, logical_oob[0], (u8)0xff);
	KUNIT_ASSERT_EQ(test, q3n_unpack_data_oob(logical_oob,
						   sizeof(logical_oob), &decoded), 0);
	KUNIT_EXPECT_EQ(test, decoded.slot, meta.slot);
	KUNIT_EXPECT_EQ(test, decoded.stripe_id, meta.stripe_id);
	KUNIT_EXPECT_EQ(test, decoded.data_crc, meta.data_crc);
	KUNIT_EXPECT_MEMEQ(test, logical_oob + 1, &decoded, sizeof(decoded));
	KUNIT_EXPECT_EQ(test, logical_oob[1 + sizeof(decoded)], (u8)0xff);
}

static void q3n_data_oob_rejects_corrupt_crc_fields_test(struct kunit *test)
{
	u8 logical_oob[128];
	struct q3n_data_meta meta = {
		.slot = 1,
		.stripe_id = cpu_to_le64(17),
		.data_crc = cpu_to_le32(0x11223344),
	};
	struct q3n_data_meta decoded;

	KUNIT_ASSERT_EQ(test, q3n_pack_data_oob(logical_oob,
						 sizeof(logical_oob), &meta), 0);
	logical_oob[1 + offsetof(struct q3n_data_meta, data_crc)] ^= 1;
	KUNIT_EXPECT_EQ(test, q3n_unpack_data_oob(logical_oob,
						   sizeof(logical_oob), &decoded),
			-EBADMSG);

	KUNIT_ASSERT_EQ(test, q3n_pack_data_oob(logical_oob,
						 sizeof(logical_oob), &meta), 0);
	logical_oob[1 + offsetof(struct q3n_data_meta, header_crc)] ^= 1;
	KUNIT_EXPECT_EQ(test, q3n_unpack_data_oob(logical_oob,
						   sizeof(logical_oob), &decoded),
			-EBADMSG);
}

static void q3n_parity_oob_round_trip_and_crc_validation_test(struct kunit *test)
{
	u8 logical_oob[128] = {};
	u8 parity[16] = { 0x81 };
	u8 data[16] = { 0x42 };
	struct q3n_open_stripe stripe = {
		.stripe_id = 23,
		.data_pages = 1,
		.parity = parity,
		.page_size = sizeof(parity),
	};
	struct q3n_parity_manifest manifest;
	struct q3n_parity_manifest decoded;

	KUNIT_ASSERT_EQ(test, q3n_open_stripe_update(&stripe, 0, data,
						       sizeof(data)), 0);
	stripe.data_crc[0] = 0x11223344;
	KUNIT_ASSERT_EQ(test, q3n_build_manifest(&stripe, &manifest), 0);
	KUNIT_ASSERT_EQ(test, q3n_pack_parity_oob(logical_oob,
						   sizeof(logical_oob), &manifest), 0);
	KUNIT_EXPECT_EQ(test, logical_oob[0], (u8)0xff);
	KUNIT_ASSERT_EQ(test, q3n_unpack_parity_oob(logical_oob,
						     sizeof(logical_oob), &decoded), 0);
	KUNIT_EXPECT_MEMEQ(test, &decoded, &manifest, sizeof(decoded));
	KUNIT_EXPECT_MEMEQ(test, logical_oob + 1, &manifest, sizeof(manifest));
	KUNIT_EXPECT_EQ(test, le32_to_cpu(decoded.data_crc[0]),
			stripe.data_crc[0]);
	KUNIT_EXPECT_EQ(test,
		logical_oob[1 + offsetof(struct q3n_parity_manifest, data_crc)],
		(u8)0x44);
	KUNIT_EXPECT_EQ(test,
		logical_oob[2 + offsetof(struct q3n_parity_manifest, data_crc)],
		(u8)0x33);
	KUNIT_EXPECT_EQ(test,
		logical_oob[3 + offsetof(struct q3n_parity_manifest, data_crc)],
		(u8)0x22);
	KUNIT_EXPECT_EQ(test,
		logical_oob[4 + offsetof(struct q3n_parity_manifest, data_crc)],
		(u8)0x11);

	logical_oob[1 + offsetof(struct q3n_parity_manifest, data_crc)] ^= 1;
	KUNIT_EXPECT_EQ(test, q3n_unpack_parity_oob(logical_oob,
						     sizeof(logical_oob), &decoded),
			-EBADMSG);
	KUNIT_ASSERT_EQ(test, q3n_pack_parity_oob(logical_oob,
						   sizeof(logical_oob), &manifest), 0);
	logical_oob[1 + offsetof(struct q3n_parity_manifest, parity_crc)] ^= 1;
	KUNIT_EXPECT_EQ(test, q3n_unpack_parity_oob(logical_oob,
						     sizeof(logical_oob), &decoded),
			-EBADMSG);
	KUNIT_ASSERT_EQ(test, q3n_pack_parity_oob(logical_oob,
						   sizeof(logical_oob), &manifest), 0);
	logical_oob[1 + offsetof(struct q3n_parity_manifest, header_crc)] ^= 1;
	KUNIT_EXPECT_EQ(test, q3n_unpack_parity_oob(logical_oob,
						     sizeof(logical_oob), &decoded),
			-EBADMSG);
}

static void q3n_unprotected_tombstone_round_trip_test(struct kunit *test)
{
	u8 logical_oob[128];
	struct q3n_unprotected_tombstone tombstone = {
		.reason = Q3N_UNPROTECTED_INVALID_METADATA,
		.stripe_id = cpu_to_le64(0x123456789ULL),
	};
	struct q3n_unprotected_tombstone decoded;
	struct q3n_parity_manifest manifest;

	KUNIT_ASSERT_EQ(test, q3n_pack_unprotected_oob(logical_oob,
						       sizeof(logical_oob),
						       &tombstone), 0);
	KUNIT_EXPECT_EQ(test, logical_oob[0], (u8)0xff);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(logical_oob + 1),
			(u16)Q3N_RAID_TOMBSTONE_MAGIC);
	KUNIT_ASSERT_EQ(test, q3n_unpack_unprotected_oob(logical_oob,
							 sizeof(logical_oob),
							 &decoded), 0);
	KUNIT_EXPECT_EQ(test, decoded.reason, tombstone.reason);
	KUNIT_EXPECT_EQ(test, decoded.stripe_id, tombstone.stripe_id);
	KUNIT_EXPECT_EQ(test, q3n_unpack_parity_oob(logical_oob,
						   sizeof(logical_oob),
						   &manifest), -EBADMSG);

	logical_oob[1 + offsetof(struct q3n_unprotected_tombstone,
				 header_crc)] ^= 1;
	KUNIT_EXPECT_EQ(test, q3n_unpack_unprotected_oob(logical_oob,
							 sizeof(logical_oob),
							 &decoded), -EBADMSG);
}

static void q3n_oob_helpers_reject_short_buffers_test(struct kunit *test)
{
	u8 logical_oob[128];
	struct q3n_data_meta data_meta = {};
	struct q3n_parity_manifest manifest = {};

	KUNIT_EXPECT_EQ(test, q3n_pack_data_oob(logical_oob,
						 sizeof(data_meta), &data_meta), -EINVAL);
	KUNIT_EXPECT_EQ(test, q3n_unpack_data_oob(logical_oob,
						   sizeof(data_meta), &data_meta), -EINVAL);
	KUNIT_EXPECT_EQ(test, q3n_pack_parity_oob(logical_oob,
						   sizeof(manifest), &manifest), -EINVAL);
	KUNIT_EXPECT_EQ(test, q3n_unpack_parity_oob(logical_oob,
						     sizeof(manifest), &manifest), -EINVAL);
}

static void q3n_open_stripe_is_unprotected_until_parity_completes_test(struct kunit *test)
{
	u8 parity[16] = {};
	u8 data[16] = { 0x1 };
	struct q3n_open_stripe stripe = {
		.stripe_id = 99,
		.data_pages = 1,
		.parity = parity,
		.page_size = sizeof(parity),
	};

	KUNIT_ASSERT_EQ(test, q3n_open_stripe_update(&stripe, 0, data,
						       sizeof(data)), 0);
	KUNIT_EXPECT_EQ(test, stripe.state, Q3N_STRIPE_UNPROTECTED);
	KUNIT_ASSERT_EQ(test, q3n_open_stripe_queue_parity(&stripe), 0);
	KUNIT_EXPECT_EQ(test, stripe.state, Q3N_STRIPE_PARITY_QUEUED);
	q3n_open_stripe_complete_parity(&stripe, false);
	KUNIT_EXPECT_EQ(test, stripe.state, Q3N_STRIPE_UNPROTECTED);
	KUNIT_ASSERT_EQ(test, q3n_open_stripe_queue_parity(&stripe), 0);
	q3n_open_stripe_complete_parity(&stripe, true);
	KUNIT_EXPECT_EQ(test, stripe.state, Q3N_STRIPE_PROTECTED);
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
	struct q3n_block_state state = { .next_prog_page = 0 };
	struct q3n_request foreground = { .class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_READ };
	struct q3n_request parity = { .class = Q3N_REQ_PARITY_WRITE,
		.op = Q3N_REQ_PROGRAM, .block_state = &state };

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &parity), 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &foreground), 0);
	KUNIT_EXPECT_PTR_EQ(test, q3n_sched_pick_next(&sched), &foreground);
	KUNIT_EXPECT_PTR_EQ(test, q3n_sched_pick_next(&sched), &parity);
}

static void q3n_scheduler_skips_unready_foreground_program_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_block_state state = { .next_prog_page = 4 };
	struct q3n_request unready_fg = { .class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_PROGRAM, .block_state = &state, .page = 5 };
	struct q3n_request ready_parity = { .class = Q3N_REQ_PARITY_WRITE,
		.op = Q3N_REQ_PROGRAM, .block_state = &state, .page = 4 };

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &unready_fg), 0);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &ready_parity), 0);
	KUNIT_EXPECT_PTR_EQ(test, q3n_sched_pick_next(&sched), &ready_parity);
	state.next_prog_page = 5;
	KUNIT_EXPECT_PTR_EQ(test, q3n_sched_pick_next(&sched), &unready_fg);
}

static void q3n_scheduler_rejects_program_without_predecessor_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_block_state state = { .next_prog_page = 0 };
	struct q3n_request skipped = { .class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_PROGRAM, .block_state = &state, .page = 1 };

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &skipped), 0);
	KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &skipped), -ERANGE);
	KUNIT_EXPECT_TRUE(test, list_empty(&skipped.node));
}

static void q3n_scheduler_rejects_stale_program_test(struct kunit *test)
{
	struct q3n_sched sched;
	struct q3n_block_state state = { .next_prog_page = 2 };
	struct q3n_request stale = { .class = Q3N_REQ_FOREGROUND,
		.op = Q3N_REQ_PROGRAM, .block_state = &state, .page = 1 };

	q3n_sched_init(&sched);
	KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &stale), 0);
	KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &stale), -ESTALE);
	KUNIT_EXPECT_TRUE(test, list_empty(&stale.node));
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

static struct kunit_case q3n_map_test_cases[] = {
	KUNIT_CASE(q3n_map_separate_parity_block_test),
	KUNIT_CASE(q3n_map_non_power_of_two_geometry_test),
	KUNIT_CASE(q3n_map_rejects_invalid_input_test),
	KUNIT_CASE(q3n_replay_serial_frontier_test),
	KUNIT_CASE(q3n_program_order_test),
	KUNIT_CASE(q3n_serial_same_block_mapping_test),
	KUNIT_CASE(q3n_incremental_xor_test),
	KUNIT_CASE(q3n_manifest_rejects_header_crc_corruption_test),
	KUNIT_CASE(q3n_data_oob_round_trip_preserves_bbm_test),
	KUNIT_CASE(q3n_data_oob_rejects_corrupt_crc_fields_test),
	KUNIT_CASE(q3n_parity_oob_round_trip_and_crc_validation_test),
	KUNIT_CASE(q3n_unprotected_tombstone_round_trip_test),
	KUNIT_CASE(q3n_oob_helpers_reject_short_buffers_test),
	KUNIT_CASE(q3n_open_stripe_is_unprotected_until_parity_completes_test),
	KUNIT_CASE(q3n_recover_single_missing_page_test),
	KUNIT_CASE(q3n_rebuild_processes_one_member_per_step_test),
	KUNIT_CASE(q3n_rebuild_rejects_stale_generation_test),
	KUNIT_CASE(q3n_scheduler_prioritizes_ready_foreground_test),
	KUNIT_CASE(q3n_scheduler_skips_unready_foreground_program_test),
	KUNIT_CASE(q3n_scheduler_rejects_program_without_predecessor_test),
	KUNIT_CASE(q3n_scheduler_rejects_stale_program_test),
	KUNIT_CASE(q3n_scheduler_prioritizes_parity_read_test),
	KUNIT_CASE(q3n_scheduler_foreground_preempts_requeued_rebuild_test),
	KUNIT_CASE(q3n_scheduler_p1_claim_preserves_foreground_test),
	KUNIT_CASE(q3n_scheduler_reserves_capacity_per_stripe_test),
	KUNIT_CASE(q3n_scheduler_tracks_max_pending_test),
	KUNIT_CASE(q3n_scheduler_waits_for_state_change_test),
	KUNIT_CASE(q3n_block_barrier_blocks_new_work_and_drains_test),
	KUNIT_CASE(q3n_block_barrier_terminal_put_wakes_on_drain_test),
	{}
};

static struct kunit_suite q3n_map_test_suite = {
	.name = "qemu-3dnand-map",
	.test_cases = q3n_map_test_cases,
};

kunit_test_suite(q3n_map_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for the QEMU 3D NAND mapper");
