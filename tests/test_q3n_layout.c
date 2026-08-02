#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "qemu_3dnand_layout.h"

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

static void expect_profile(const char *name, bool raid_enabled, uint32_t data_pages,
			   uint32_t writesize, uint32_t oobsize,
			   uint32_t pages_per_block, uint32_t tail_pages,
			   uint64_t erasesize, uint64_t chipsize_mib)
{
	const struct q3n_geometry physical = {
		.writesize = 16384,
		.oobsize = 1024,
		.pages_per_block = 1600,
		.blocks = 1664,
	};
	struct q3n_page_profile profile;
	int ret;

	ret = q3n_layout_build(&profile, &physical, raid_enabled, data_pages);
	expect_u64(name, ret, 0);
	expect_u64("logical writesize", profile.logical.writesize, writesize);
	expect_u64("logical oobsize", profile.logical.oobsize, oobsize);
	expect_u64("logical pages per block", profile.logical.pages_per_block,
		   pages_per_block);
	expect_u64("tail pages", profile.tail_pages, tail_pages);
	expect_u64("logical erasesize",
		   (uint64_t)profile.logical.writesize * profile.logical.pages_per_block,
		   erasesize);
	expect_u64("logical chipsize MiB", profile.logical_size / (1024 * 1024),
		   chipsize_mib);
}

static void expect_map(const struct q3n_page_profile *profile, uint32_t page,
		       uint32_t first_data_page, uint32_t parity_page)
{
	struct q3n_page_map map;
	int ret;

	ret = q3n_layout_map_page(profile, page, &map);
	expect_u64("map return", ret, 0);
	expect_u64("map logical block", map.logical_block,
		   page / profile->logical.pages_per_block);
	expect_u64("map stripe", map.stripe_in_block,
		   page % profile->logical.pages_per_block);
	expect_u64("map first data page", map.first_data_page, first_data_page);
	expect_u64("map parity page", map.parity_page, parity_page);
}

static void expect_map_error(const char *name,
			     const struct q3n_page_profile *profile,
			     uint32_t page)
{
	struct q3n_page_map map = { 0 };
	int ret;

	ret = q3n_layout_map_page(profile, page, &map);
	expect_error(name, ret);
}

int main(void)
{
	const struct q3n_geometry physical = {
		.writesize = 16384,
		.oobsize = 1024,
		.pages_per_block = 1600,
		.blocks = 1664,
	};
	const struct q3n_geometry zero = { 0 };
	const uint32_t invalid_data_pages[] = { 0, 1, 3, 5, 6, 7, 16 };
	struct q3n_page_profile profile;
	uint32_t i;
	int ret;

	expect_profile("disabled profile", false, 8, 16384, 1024, 1600, 0,
		       26214400, 41600);
	ret = q3n_layout_build(&profile, &physical, false, 8);
	expect_u64("disabled setup", ret, 0);
	expect_u64("disabled data pages", profile.data_pages, 1);
	expect_u64("disabled parity pages", profile.parity_pages, 0);
	expect_u64("disabled stripe pages", profile.stripe_pages, 1);
	expect_u64("disabled physical geometry copied", profile.physical.writesize,
		   physical.writesize);
	expect_u64("disabled logical geometry copied", profile.logical.writesize,
		   physical.writesize);
	expect_map(&profile, 1600, 1600, 1600);
	expect_map(&profile, 2662399, 2662399, 2662399);
	expect_map_error("disabled one-past-end page accepted", &profile, 2662400);
	expect_profile("2:1 profile", true, 2, 32768, 2048, 533, 1,
		       17465344, 27716);
	expect_profile("4:1 profile", true, 4, 65536, 4096, 320, 0,
		       20971520, 33280);
	expect_profile("8:1 profile", true, 8, 131072, 8192, 177, 7,
		       23199744, 36816);

	for (i = 0; i < sizeof(invalid_data_pages) / sizeof(invalid_data_pages[0]); i++) {
		ret = q3n_layout_build(&profile, &physical, true, invalid_data_pages[i]);
		expect_error("invalid RAID data pages accepted", ret);
	}

	ret = q3n_layout_build(&profile, &zero, true, 4);
	expect_error("zero geometry accepted", ret);
	ret = q3n_layout_build(&profile, NULL, true, 4);
	expect_error("NULL geometry accepted", ret);
	ret = q3n_layout_build(NULL, &physical, true, 4);
	expect_error("NULL profile accepted", ret);

	{
		const struct q3n_geometry overflow = {
			.writesize = UINT32_MAX,
			.oobsize = 1024,
			.pages_per_block = 1600,
			.blocks = 1664,
		};

		ret = q3n_layout_build(&profile, &overflow, true, 2);
		expect_error("logical geometry multiplication overflow accepted", ret);
	}
	{
		const struct q3n_geometry overflow = {
			.writesize = 4,
			.oobsize = 1,
			.pages_per_block = UINT32_MAX,
			.blocks = UINT32_MAX,
		};

		ret = q3n_layout_build(&profile, &overflow, false, 1);
		expect_error("oversized physical page map accepted", ret);
	}

	ret = q3n_layout_build(&profile, &physical, true, 4);
	expect_u64("4:1 setup", ret, 0);
	expect_map(&profile, 0, 0, 4);
	expect_map(&profile, 319, 1595, 1599);
	expect_map(&profile, 320, 1600, 1604);
	expect_map(&profile, 532479, 2662395, 2662399);
	expect_map_error("4:1 one-past-end page accepted", &profile, 532480);

	ret = q3n_layout_build(&profile, &physical, true, 8);
	expect_u64("8:1 setup", ret, 0);
	expect_map(&profile, 176, 1584, 1592);
	expect_map(&profile, 177, 1600, 1608);
	expect_map(&profile, 294527, 2662384, 2662392);
	expect_map_error("8:1 one-past-end page accepted", &profile, 294528);

	ret = q3n_layout_build(&profile, &physical, true, 2);
	expect_u64("2:1 setup", ret, 0);
	expect_map(&profile, 532, 1596, 1598);
	expect_map(&profile, 533, 1600, 1602);
	expect_map(&profile, 886911, 2662396, 2662398);
	expect_map_error("2:1 one-past-end page accepted", &profile, 886912);

	ret = q3n_layout_map_page(&profile,
			profile.logical.pages_per_block * profile.logical.blocks, NULL);
	expect_error("NULL map accepted", ret);
	ret = q3n_layout_map_page(&profile,
			profile.logical.pages_per_block * profile.logical.blocks,
			&(struct q3n_page_map) { 0 });
	expect_error("out-of-range logical page accepted", ret);

	ret = q3n_layout_build(&profile, &physical, true, 4);
	expect_u64("mutated-profile setup", ret, 0);
	{
		struct q3n_page_profile mutated = profile;

		mutated.data_pages = 3;
		expect_map_error("unsupported RAID data-page count accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.data_pages = 6;
		expect_map_error("non-power-of-two RAID data-page count accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.data_pages = 16;
		expect_map_error("out-of-profile power-of-two data-page count accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.parity_pages = 2;
		expect_map_error("RAID profile with two parity pages accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.stripe_pages = profile.data_pages + 2;
		expect_map_error("inconsistent RAID stripe width accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.logical.blocks--;
		expect_map_error("inconsistent logical block count accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.logical.pages_per_block--;
		expect_map_error("inconsistent logical pages per block accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.physical.pages_per_block--;
		expect_map_error("inconsistent physical pages per block accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.logical.writesize--;
		expect_map_error("inconsistent logical page size accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.logical_size--;
		expect_map_error("inconsistent logical capacity accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.logical.writesize = 0;
		expect_map_error("zero logical geometry accepted", &mutated, 0);
		mutated = profile;
		mutated.physical.pages_per_block = 0;
		expect_map_error("zero physical geometry accepted", &mutated, 0);
		mutated = profile;
		mutated.physical.blocks = UINT32_MAX;
		expect_map_error("overflowing physical geometry accepted",
				 &mutated, 0);
		mutated = profile;
		mutated.logical.pages_per_block = UINT32_MAX;
		mutated.logical.blocks = UINT32_MAX;
		expect_map_error("overflowing logical geometry accepted",
				 &mutated, 0);
	}
	{
		struct q3n_page_profile cross_block = profile;

		cross_block.stripes_per_block = 321;
		cross_block.logical.pages_per_block = 321;
		cross_block.tail_pages = 0;
		expect_map_error("stripe crossing its physical block accepted",
				 &cross_block, 320);
	}

	printf("ok: Q3N page RAID layout profiles verified\n");
	return 0;
}
