#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "qemu_3dnand_multiplane_layout.h"

static void fail(const char *name, uint64_t got, uint64_t want)
{
	fprintf(stderr, "FAIL: %s: got %llu, want %llu\n", name,
		(unsigned long long)got, (unsigned long long)want);
	exit(1);
}

static void expect_u64(const char *name, uint64_t got, uint64_t want)
{
	if (got != want)
		fail(name, got, want);
}

static void expect_error(const char *name, int ret)
{
	if (ret >= 0)
		fail(name, ret, -1);
}

static void expect_blocks(const struct q3n_multiplane_profile *profile,
			  uint32_t logical_block, uint32_t first,
			  uint32_t second, uint32_t third, uint32_t fourth)
{
	const uint32_t wanted[] = { first, second, third, fourth };
	struct q3n_mp_addr addr;
	uint32_t plane;
	uint32_t block;
	int ret;

	ret = q3n_multiplane_map_block(profile, logical_block, &addr);
	expect_u64("map block return", ret, 0);
	expect_u64("map block page", addr.page_in_block, 0);
	for (plane = 0; plane < Q3N_MP_PLANES; plane++) {
		ret = q3n_multiplane_physical_block(profile, &addr, plane, &block);
		expect_u64("physical block return", ret, 0);
		expect_u64("physical block", block, wanted[plane]);
	}
}

static void expect_page(const struct q3n_multiplane_profile *profile,
			uint32_t logical_page, uint32_t die, uint32_t block,
			uint32_t page)
{
	struct q3n_mp_addr addr;
	int ret;

	ret = q3n_multiplane_map_page(profile, logical_page, &addr);
	expect_u64("map page return", ret, 0);
	expect_u64("map page die", addr.die, die);
	expect_u64("map page block", addr.block_in_plane, block);
	expect_u64("map page page", addr.page_in_block, page);
}

int main(void)
{
	const struct q3n_geometry physical = {
		.writesize = 16384,
		.oobsize = 1024,
		.pages_per_block = 1600,
		.blocks = 1664,
	};
	const struct q3n_flash_topology topology = {
		.dies = 2,
		.planes_per_die = 4,
		.blocks_per_plane = 247,
		.data_blocks_per_plane = 208,
		.pages_per_block = 1600,
	};
	struct q3n_multiplane_profile profile;
	struct q3n_multiplane_profile mutated;
	struct q3n_mp_addr addr;
	int ret;

	ret = q3n_multiplane_layout_build(&profile, &physical, &topology);
	expect_u64("build profile", ret, 0);
	expect_u64("logical writesize", profile.logical.writesize, 65536);
	expect_u64("logical oobsize", profile.logical.oobsize, 4096);
	expect_u64("logical pages per block", profile.logical.pages_per_block, 1600);
	expect_u64("logical blocks", profile.logical.blocks, 416);
	expect_u64("logical size", profile.logical_size, 43620761600ULL);
	expect_blocks(&profile, 0, 0, 247, 494, 741);
	expect_blocks(&profile, 1, 988, 1235, 1482, 1729);
	expect_blocks(&profile, 414, 207, 454, 701, 948);
	expect_blocks(&profile, 415, 1195, 1442, 1689, 1936);
	expect_page(&profile, 1599, 0, 0, 1599);
	expect_page(&profile, 1600, 1, 0, 0);
	expect_page(&profile, 665599, 1, 207, 1599);
	ret = q3n_multiplane_map_page(&profile, 665600, &addr);
	expect_u64("one-past-end page", ret, -ERANGE);

	ret = q3n_multiplane_layout_build(NULL, &physical, &topology);
	expect_error("NULL profile accepted", ret);
	ret = q3n_multiplane_layout_build(&profile, NULL, &topology);
	expect_error("NULL geometry accepted", ret);
	ret = q3n_multiplane_layout_build(&profile, &physical, NULL);
	expect_error("NULL topology accepted", ret);
	ret = q3n_multiplane_layout_build(&profile, &(struct q3n_geometry) { 0 },
					 &topology);
	expect_error("zero geometry accepted", ret);
	ret = q3n_multiplane_layout_build(&profile, &physical,
					 &(struct q3n_flash_topology) {
						.dies = 1, .planes_per_die = 4,
						.blocks_per_plane = 247,
						.data_blocks_per_plane = 208,
						.pages_per_block = 1600,
					 });
	expect_error("one die accepted", ret);
	ret = q3n_multiplane_layout_build(&profile, &physical,
					 &(struct q3n_flash_topology) {
						.dies = 2, .planes_per_die = 3,
						.blocks_per_plane = 247,
						.data_blocks_per_plane = 208,
						.pages_per_block = 1600,
					 });
	expect_error("three planes accepted", ret);
	ret = q3n_multiplane_layout_build(&profile, &physical,
					 &(struct q3n_flash_topology) {
						.dies = 2, .planes_per_die = 4,
						.blocks_per_plane = 247,
						.data_blocks_per_plane = 248,
						.pages_per_block = 1600,
					 });
	expect_error("more data blocks than physical blocks accepted", ret);
	ret = q3n_multiplane_layout_build(&profile, &physical,
					 &(struct q3n_flash_topology) {
						.dies = 2, .planes_per_die = 4,
						.blocks_per_plane = 247,
						.data_blocks_per_plane = 208,
						.pages_per_block = 1599,
					 });
	expect_error("topology pages per block mismatch accepted", ret);
	ret = q3n_multiplane_layout_build(&profile,
					 &(struct q3n_geometry) {
						.writesize = UINT32_MAX,
						.oobsize = UINT32_MAX,
						.pages_per_block = UINT32_MAX,
						.blocks = UINT32_MAX,
					 }, &(struct q3n_flash_topology) {
						.dies = 2, .planes_per_die = 4,
						.blocks_per_plane = UINT32_MAX,
						.data_blocks_per_plane = UINT32_MAX,
						.pages_per_block = UINT32_MAX,
					 });
	expect_error("overflowing profile accepted", ret);

	mutated = profile;
	mutated.data_blocks_per_plane = 209;
	ret = q3n_multiplane_map_block(&mutated, 0, &addr);
	expect_error("mutated data-block count accepted", ret);
	ret = q3n_multiplane_map_block(&profile, 416, &addr);
	expect_u64("one-past-end block", ret, -ERANGE);
	ret = q3n_multiplane_map_block(&profile, 0, NULL);
	expect_error("NULL map block address accepted", ret);
	ret = q3n_multiplane_physical_block(&profile, &addr, 4, &(uint32_t) { 0 });
	expect_error("fifth plane accepted", ret);
	addr.die = 0;
	addr.block_in_plane = 208;
	addr.page_in_block = 0;
	ret = q3n_multiplane_physical_block(&profile, &addr, 0, &(uint32_t) { 0 });
	expect_error("out-of-range plane block accepted", ret);

	printf("ok: Q3N four-plane layout mapping verified\n");
	return 0;
}
