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
	struct q3n_ecc_result result = {
		.status = Q3N_ECC_STATUS_CORRECTED,
		.max_bitflips = 12,
		.corrected_bits = 17,
	};
	int ret;

	ret = q3n_ecc_account_result(&result, 0, &stats);
	expect("normal corrected return", ret, 12);
	expect("corrected accounting", stats.corrected, 17);
	expect("failed remains zero", stats.failed, 0);

	result.status = Q3N_ECC_STATUS_UNCORRECTABLE;
	result.max_bitflips = 0;
	result.corrected_bits = 0;
	ret = q3n_ecc_account_result(&result, 0, &stats);
	expect("uncorrectable returns retry signal", ret, Q3N_ECC_STRENGTH);
	expect("uncorrectable increments failed", stats.failed, 1);

	result.status = Q3N_ECC_STATUS_CORRECTED;
	result.max_bitflips = 33;
	result.corrected_bits = 41;
	ret = q3n_ecc_account_result(&result, 1, &stats);
	expect("retry success reaches threshold", ret, Q3N_ECC_STRENGTH);
	expect("retry corrected accounting", stats.corrected, 58);
	expect("retry does not add failure", stats.failed, 1);

	printf("ok: Q3N ECC retry accounting verified\n");
	return 0;
}
