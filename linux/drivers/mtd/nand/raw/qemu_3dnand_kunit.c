// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

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

static void q3n_program_order_test(struct kunit *test)
{
	struct q3n_block_state state = { .next_prog_page = 17 };

	KUNIT_EXPECT_TRUE(test, q3n_program_order_ready(&state, 17));
	KUNIT_EXPECT_FALSE(test, q3n_program_order_ready(&state, 16));
	KUNIT_EXPECT_FALSE(test, q3n_program_order_ready(&state, 18));
}

static struct kunit_case q3n_map_test_cases[] = {
	KUNIT_CASE(q3n_map_separate_parity_block_test),
	KUNIT_CASE(q3n_map_non_power_of_two_geometry_test),
	KUNIT_CASE(q3n_map_rejects_invalid_input_test),
	KUNIT_CASE(q3n_program_order_test),
	{}
};

static struct kunit_suite q3n_map_test_suite = {
	.name = "qemu-3dnand-map",
	.test_cases = q3n_map_test_cases,
};

kunit_test_suite(q3n_map_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for the QEMU 3D NAND mapper");
