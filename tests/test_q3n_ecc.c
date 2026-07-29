#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "qemu_3dnand_ecc.h"
#include "qemu_3dnand_regs.h"

static void expect(const char *name, uint64_t got, uint64_t want)
{
	if (got != want) {
		fprintf(stderr, "FAIL: %s: got %llu, want %llu\n", name,
			(unsigned long long)got, (unsigned long long)want);
		exit(1);
	}
}

int main(void)
{
	struct q3n_ecc_stats stats = { 0 };
	struct q3n_page_result result = { 0 };
	int ret;

	ret = q3n_ecc_account_page_result(&result, 0, &stats);
	expect("clean return", ret, 0);
	expect("clean corrected accounting", stats.corrected, 0);
	expect("clean failed accounting", stats.failed, 0);

	result = (struct q3n_page_result) {
		.max_bitflips = 12,
		.corrected_bits = 17,
	};
	ret = q3n_ecc_account_page_result(&result, 0, &stats);
	expect("normal corrected return", ret, 12);
	expect("corrected accounting", stats.corrected, 17);
	expect("failed remains zero", stats.failed, 0);

	result = (struct q3n_page_result) {
		.failed_data_pages = 1,
	};
	ret = q3n_ecc_account_page_result(&result, 0, &stats);
	expect("uncorrectable returns retry signal", ret, Q3N_ECC_STRENGTH);
	expect("uncorrectable increments failed", stats.failed, 1);

	result = (struct q3n_page_result) {
		.failed_data_pages = 2,
	};
	ret = q3n_ecc_account_page_result(&result, 3, &stats);
	expect("two failed data pages return threshold", ret, Q3N_ECC_STRENGTH);
	expect("two failed data pages count one logical failure", stats.failed, 2);

	result = (struct q3n_page_result) {
		.max_bitflips = Q3N_ECC_STRENGTH,
		.recovered = true,
	};
	ret = q3n_ecc_account_page_result(&result, 3, &stats);
	expect("RAID recovery returns threshold", ret, Q3N_ECC_STRENGTH);
	expect("RAID recovery keeps failed accounting", stats.failed, 2);

	result = (struct q3n_page_result) {
		.max_bitflips = 7,
		.recovered = true,
	};
	ret = q3n_ecc_account_page_result(&result, 3, &stats);
	expect("RAID recovery below strength returns threshold", ret,
	       Q3N_ECC_STRENGTH);
	expect("RAID recovery below strength keeps failed accounting",
	       stats.failed, 2);

	result = (struct q3n_page_result) {
		.max_bitflips = 33,
		.corrected_bits = 41,
	};
	ret = q3n_ecc_account_page_result(&result, 1, &stats);
	expect("retry success reaches threshold", ret, Q3N_ECC_STRENGTH);
	expect("retry corrected accounting", stats.corrected, 58);
	expect("retry does not add failure", stats.failed, 2);

	printf("ok: Q3N ECC retry accounting verified\n");
	return 0;
}
