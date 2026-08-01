#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu_3dnand_flash.h"
#include "qemu_3dnand_layout.h"
#include "qemu_3dnand_multiplane_layout.h"
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
	const struct q3n_flash_info *info;
	const struct nand_flash_dev *ids;
	const struct q3n_geometry physical = {
		.writesize = 16384,
		.oobsize = 1024,
		.pages_per_block = 1600,
		.blocks = 1664,
	};
	struct q3n_page_profile profile;
	struct q3n_multiplane_profile multiplane;
	struct q3n_geometry invalid;
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

	info = q3n_flash_info_for_id(expected_id, sizeof(expected_id));
	if (!info)
		fail("full YTMC ID was not selected");
	if (info != ytmc_nand_flash_info())
		fail("selector did not return the YTMC table");
	ids = &info->nand;
	if (ids[0].id_len != YTMC_Q3N_ID_LEN ||
	    memcmp(ids[0].id, expected_id, sizeof(expected_id)))
		fail("YTMC table does not preserve the full ID");
	if (ids[0].pagesize != 16384 || ids[0].oobsize != 1024 ||
	    ids[0].erasesize != 26214400 || ids[0].chipsize != 41600)
		fail("YTMC geometry does not match the 40.625 GiB device");
	if (!(ids[0].options & NAND_NON_POWER_OF_2_GEOMETRY))
		fail("YTMC profile does not request exact geometry");
	if (info[1].nand.name)
		fail("YTMC ID table lacks its sentinel");
	if (info->topology.dies != 2 || info->topology.planes_per_die != 4 ||
	    info->topology.blocks_per_plane != 247 ||
	    info->topology.data_blocks_per_plane != 208 ||
	    info->topology.pages_per_block != 1600)
		fail("YTMC topology does not match the Q3N device");

	if (q3n_flash_info_for_id(expected_id, YTMC_Q3N_ID_LEN - 1))
		fail("seven-byte prefix was accepted");
	if (q3n_flash_info_for_id(NULL, YTMC_Q3N_ID_LEN))
		fail("NULL ID was accepted");
	if (q3n_flash_info_for_id(expected_id, 0))
		fail("zero-length ID was accepted");

	for (i = 0; i < sizeof(candidate); i++) {
		memcpy(candidate, expected_id, sizeof(candidate));
		candidate[i] ^= 0x01;
		if (q3n_flash_info_for_id(candidate, sizeof(candidate)))
			fail("one-byte ID mutation was accepted");
	}

	for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
		if (q3n_layout_build(&profile, &physical, profiles[i].raid_enabled,
				     profiles[i].data_pages))
			fail("could not build layout profile");
		if (q3n_flash_build_scan_ids(scan_ids, info, &profile.logical))
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
		if (memcmp(&scan_ids[1], &(struct nand_flash_dev) { 0 },
			   sizeof(scan_ids[1])))
			fail("scan ID table lacks an empty sentinel");
	}

	if (q3n_multiplane_layout_build(&multiplane, &physical,
				       &info->topology))
		fail("could not build multi-plane layout profile");
	if (q3n_flash_build_scan_ids(scan_ids, info, &multiplane.logical))
		fail("could not build multi-plane scan IDs");
	if (scan_ids[0].pagesize != 65536 || scan_ids[0].oobsize != 4096 ||
	    scan_ids[0].erasesize != 104857600 || scan_ids[0].chipsize != 41600)
		fail("multi-plane scan IDs did not use exact logical geometry");
	if (memcmp(scan_ids[0].id, expected_id, sizeof(expected_id)) ||
	    scan_ids[0].id_len != YTMC_Q3N_ID_LEN ||
	    !(scan_ids[0].options & NAND_NON_POWER_OF_2_GEOMETRY))
		fail("multi-plane scan IDs did not preserve NAND identity");
	if (memcmp(&scan_ids[1], &(struct nand_flash_dev) { 0 },
		   sizeof(scan_ids[1])))
		fail("multi-plane scan IDs lack an empty sentinel");

	if (q3n_flash_build_scan_ids(NULL, info, &profile.logical) >= 0 ||
	    q3n_flash_build_scan_ids(scan_ids, NULL, &profile.logical) >= 0 ||
	    q3n_flash_build_scan_ids(scan_ids, info, NULL) >= 0)
		fail("NULL scan-ID argument accepted");

	invalid = (struct q3n_geometry) {
		.writesize = 1,
		.oobsize = 1,
		.pages_per_block = 1,
		.blocks = 1,
	};
	if (q3n_flash_build_scan_ids(scan_ids, info, &invalid) >= 0)
		fail("non-integral MiB logical capacity accepted");
	invalid = (struct q3n_geometry) {
		.writesize = 2 * 1024 * 1024,
		.oobsize = 1,
		.pages_per_block = 1,
		.blocks = UINT32_MAX,
	};
	if (q3n_flash_build_scan_ids(scan_ids, info, &invalid) >= 0)
		fail("oversized chipsize accepted");
	invalid = (struct q3n_geometry) {
		.writesize = 2,
		.oobsize = 1,
		.pages_per_block = UINT32_MAX,
		.blocks = 1,
	};
	if (q3n_flash_build_scan_ids(scan_ids, info, &invalid) >= 0)
		fail("oversized erasesize accepted");
	invalid = (struct q3n_geometry) {
		.writesize = 16384,
		.oobsize = (uint32_t)UINT16_MAX + 1,
		.pages_per_block = 1600,
		.blocks = 1664,
	};
	if (q3n_flash_build_scan_ids(scan_ids, info, &invalid) >= 0)
		fail("oversized OOB size accepted");

	printf("ok: YTMC full-ID whitelist, geometry and scan IDs verified\n");
	return 0;
}
