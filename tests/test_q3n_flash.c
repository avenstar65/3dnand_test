#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu_3dnand_flash.h"
#include "qemu_3dnand_layout.h"
#include "ytmc_nand.h"

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: %s\n", message);
	exit(1);
}

int main(void)
{
	static const uint8_t expected_id[YTMC_Q3N_ID_LEN] = {
		0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44,
	};
	uint8_t candidate[YTMC_Q3N_ID_LEN];
	const struct nand_flash_dev *ids;
	const struct q3n_geometry physical = {
		.writesize = 16384,
		.oobsize = 1024,
		.pages_per_block = 1600,
		.blocks = 1664,
	};
	struct q3n_page_profile profile;
	struct nand_flash_dev scan_ids[2];
	const struct {
		bool raid_enabled;
		uint32_t data_pages;
		uint32_t pagesize;
		uint16_t oobsize;
		unsigned int erasesize;
		unsigned int chipsize;
	} profiles[] = {
		{ false, 1, 16384, 1024, 26214400, 41600 },
		{ true, 2, 32768, 2048, 17465344, 27716 },
		{ true, 4, 65536, 4096, 20971520, 33280 },
		{ true, 8, 131072, 8192, 23199744, 36816 },
	};
	size_t i;

	ids = q3n_flash_ids_for_id(expected_id, sizeof(expected_id));
	if (!ids)
		fail("full YTMC ID was not selected");
	if (ids != ytmc_nand_ids())
		fail("selector did not return the YTMC table");
	if (ids[0].id_len != YTMC_Q3N_ID_LEN ||
	    memcmp(ids[0].id, expected_id, sizeof(expected_id)))
		fail("YTMC table does not preserve the full ID");
	if (ids[0].pagesize != 16384 || ids[0].oobsize != 1024 ||
	    ids[0].erasesize != 26214400 || ids[0].chipsize != 41600)
		fail("YTMC geometry does not match the 40.625 GiB device");
	if (!(ids[0].options & NAND_NON_POWER_OF_2_GEOMETRY))
		fail("YTMC profile does not request exact geometry");
	if (ids[1].name)
		fail("YTMC ID table lacks its sentinel");

	if (q3n_flash_ids_for_id(expected_id, YTMC_Q3N_ID_LEN - 1))
		fail("seven-byte prefix was accepted");
	if (q3n_flash_ids_for_id(NULL, YTMC_Q3N_ID_LEN))
		fail("NULL ID was accepted");
	if (q3n_flash_ids_for_id(expected_id, 0))
		fail("zero-length ID was accepted");

	for (i = 0; i < sizeof(candidate); i++) {
		memcpy(candidate, expected_id, sizeof(candidate));
		candidate[i] ^= 0x01;
		if (q3n_flash_ids_for_id(candidate, sizeof(candidate)))
			fail("one-byte ID mutation was accepted");
	}

	for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
		if (q3n_layout_build(&profile, &physical, profiles[i].raid_enabled,
				     profiles[i].data_pages))
			fail("could not build layout profile");
		if (q3n_flash_build_scan_ids(scan_ids, ids, &profile))
			fail("could not build device-local scan IDs");
		if (memcmp(scan_ids[0].id, ids[0].id, YTMC_Q3N_ID_LEN) ||
		    scan_ids[0].id_len != ids[0].id_len ||
		    scan_ids[0].ecc.strength_ds != ids[0].ecc.strength_ds ||
		    scan_ids[0].ecc.step_ds != ids[0].ecc.step_ds ||
		    scan_ids[0].options != ids[0].options ||
		    strcmp(scan_ids[0].name, ids[0].name))
			fail("scan ID derivation did not copy physical ID fields");
		if (scan_ids[0].pagesize != profiles[i].pagesize ||
		    scan_ids[0].oobsize != profiles[i].oobsize ||
		    scan_ids[0].erasesize != profiles[i].erasesize ||
		    scan_ids[0].chipsize != profiles[i].chipsize)
			fail("scan ID derivation did not replace logical geometry");
		if (scan_ids[1].name || scan_ids[1].id_len || scan_ids[1].pagesize ||
		    scan_ids[1].chipsize || scan_ids[1].erasesize || scan_ids[1].options ||
		    scan_ids[1].oobsize)
			fail("scan ID table lacks an empty sentinel");
	}

	if (q3n_flash_build_scan_ids(NULL, ids, &profile) >= 0 ||
	    q3n_flash_build_scan_ids(scan_ids, NULL, &profile) >= 0 ||
	    q3n_flash_build_scan_ids(scan_ids, ids, NULL) >= 0)
		fail("NULL scan-ID argument accepted");

	profile.logical_size++;
	if (q3n_flash_build_scan_ids(scan_ids, ids, &profile) >= 0)
		fail("non-integral MiB logical capacity accepted");
	profile.logical_size = ((uint64_t)UINT32_MAX + 1) * 1024 * 1024;
	if (q3n_flash_build_scan_ids(scan_ids, ids, &profile) >= 0)
		fail("oversized chipsize accepted");
	profile.logical_size = 1024 * 1024;
	profile.logical.writesize = 2;
	profile.logical.pages_per_block = UINT32_MAX;
	if (q3n_flash_build_scan_ids(scan_ids, ids, &profile) >= 0)
		fail("oversized erasesize accepted");
	profile.logical.writesize = 16384;
	profile.logical.pages_per_block = 1600;
	profile.logical.oobsize = (uint32_t)UINT16_MAX + 1;
	if (q3n_flash_build_scan_ids(scan_ids, ids, &profile) >= 0)
		fail("oversized OOB size accepted");

	printf("ok: YTMC full-ID whitelist, geometry and scan IDs verified\n");
	return 0;
}
