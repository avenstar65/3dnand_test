#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu_3dnand_flash.h"
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

	printf("ok: YTMC full-ID whitelist and geometry verified\n");
	return 0;
}
