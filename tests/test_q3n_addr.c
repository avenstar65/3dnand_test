#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "qemu_3dnand_addr.h"

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

static void expect_ok_page(const struct q3n_geometry *geo, uint32_t page,
			   uint32_t want_block, uint32_t want_page)
{
	uint32_t block = UINT32_MAX;
	uint32_t page_in_block = UINT32_MAX;
	int ret;

	ret = q3n_page_to_address(geo, page, &block, &page_in_block);
	expect_u64("page conversion return", ret, 0);
	expect_u64("page conversion block", block, want_block);
	expect_u64("page conversion page", page_in_block, want_page);
}

int main(void)
{
	const struct q3n_geometry geo = {
		.writesize = 16384,
		.oobsize = 1024,
		.pages_per_block = 1600,
		.blocks = 1664,
	};
	uint32_t block;
	uint32_t page;
	uint32_t column;
	int ret;

	expect_ok_page(&geo, 0, 0, 0);
	expect_ok_page(&geo, 1599, 0, 1599);
	expect_ok_page(&geo, 1600, 1, 0);
	expect_ok_page(&geo, 2662399, 1663, 1599);

	ret = q3n_page_to_address(&geo, 2662400, &block, &page);
	if (ret >= 0)
		fail("out-of-range page accepted", ret, -1);

	ret = q3n_offset_to_address(&geo, 26214400, &block, &page, &column);
	expect_u64("offset conversion return", ret, 0);
	expect_u64("offset block", block, 1);
	expect_u64("offset page", page, 0);
	expect_u64("offset column", column, 0);

	ret = q3n_offset_to_address(&geo, 26214400 + 16384 + 37,
				    &block, &page, &column);
	expect_u64("column conversion return", ret, 0);
	expect_u64("column block", block, 1);
	expect_u64("column page", page, 1);
	expect_u64("column value", column, 37);

	ret = q3n_offset_to_address(&geo, 43620761599ULL,
				    &block, &page, &column);
	expect_u64("last byte return", ret, 0);
	expect_u64("last byte block", block, 1663);
	expect_u64("last byte page", page, 1599);
	expect_u64("last byte column", column, 16383);

	ret = q3n_offset_to_address(&geo, 43620761600ULL,
				    &block, &page, &column);
	if (ret >= 0)
		fail("out-of-range offset accepted", ret, -1);

	ret = q3n_page_to_address(NULL, 0, &block, &page);
	if (ret >= 0)
		fail("NULL geometry accepted", ret, -1);

	printf("ok: exact Q3N address conversion verified\n");
	return 0;
}
