#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_priv.h"
#include "qemu_3dnand_regs.h"

struct write_event {
	uint32_t reg;
	uint32_t value;
};

struct fake_mmio {
	struct write_event writes[32];
	size_t nwrites;
	uint32_t reads[32];
	size_t nreads;
	uint32_t status;
	uint32_t capabilities;
	uint8_t id[8];
	size_t id_pos;
};

static uint32_t fake_read(void *context, uint32_t reg)
{
	struct fake_mmio *fake = context;
	uint32_t value = 0;
	size_t i;

	if (fake->nreads >= sizeof(fake->reads) / sizeof(fake->reads[0])) {
		fprintf(stderr, "FAIL: read log overflow\n");
		exit(1);
	}
	fake->reads[fake->nreads++] = reg;

	if (reg == Q3N_REG_STATUS)
		return fake->status;
	if (reg == Q3N_REG_CAP)
		return fake->capabilities;
	if (reg != Q3N_REG_DATA)
		return 0;

	for (i = 0; i < 4; i++)
		value |= (uint32_t)fake->id[fake->id_pos++] << (i * 8);
	return value;
}

static void fake_write(void *context, uint32_t reg, uint32_t value)
{
	struct fake_mmio *fake = context;

	if (fake->nwrites >= sizeof(fake->writes) / sizeof(fake->writes[0])) {
		fprintf(stderr, "FAIL: write log overflow\n");
		exit(1);
	}
	fake->writes[fake->nwrites].reg = reg;
	fake->writes[fake->nwrites].value = value;
	fake->nwrites++;
}

static void expect_write(const struct fake_mmio *fake, size_t index,
			 uint32_t reg, uint32_t value)
{
	if (index >= fake->nwrites || fake->writes[index].reg != reg ||
	    fake->writes[index].value != value) {
		fprintf(stderr,
			"FAIL: write %zu: got (%#x,%#x), want (%#x,%#x)\n",
			index,
			index < fake->nwrites ? fake->writes[index].reg : 0,
			index < fake->nwrites ? fake->writes[index].value : 0,
			reg, value);
		exit(1);
	}
}

int main(void)
{
	struct fake_mmio fake = {
		.status = Q3N_STATUS_READY,
		.capabilities = 0x12345670U,
		.id = { 0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44 },
	};
	struct q3n q3n = {
		.mmio = {
			.read = fake_read,
			.write = fake_write,
			.context = &fake,
		},
	};
	uint8_t id[8] = { 0 };
	uint8_t short_id[2] = { 0 };
	uint8_t nand_status = 0;
	size_t reads_before;
	int ret;

	ret = q3n_hw_read_id(&q3n, id, sizeof(id));
	if (ret || memcmp(id, fake.id, sizeof(id))) {
		fprintf(stderr, "FAIL: full ID read failed\n");
		return 1;
	}
	expect_write(&fake, 0, Q3N_REG_LEN, 8);
	expect_write(&fake, 1, Q3N_REG_CMD, Q3N_CMD_READ_ID);

	fake.id_pos = 0;
	ret = q3n_hw_read_id(&q3n, short_id, sizeof(short_id));
	if (ret || memcmp(short_id, fake.id, sizeof(short_id))) {
		fprintf(stderr, "FAIL: NAND Core two-byte ID read failed\n");
		return 1;
	}
	expect_write(&fake, 2, Q3N_REG_LEN, 2);
	expect_write(&fake, 3, Q3N_REG_CMD, Q3N_CMD_READ_ID);

	ret = q3n_hw_set_retry_mode(&q3n, 3);
	if (ret)
		return 1;
	expect_write(&fake, 4, Q3N_REG_RETRY_MODE, 3);

	ret = q3n_hw_set_retry_mode(&q3n, 4);
	if (ret >= 0 || fake.nwrites != 5) {
		fprintf(stderr, "FAIL: invalid retry mode reached MMIO\n");
		return 1;
	}

	reads_before = fake.nreads;
	if (Q3N_CAP_MULTIPLANE != (1U << 4)) {
		fprintf(stderr, "FAIL: multi-plane capability bit is wrong\n");
		return 1;
	}
	if (q3n_hw_read_capabilities(&q3n) != fake.capabilities ||
	    fake.nreads != reads_before + 1 ||
	    fake.reads[reads_before] != Q3N_REG_CAP) {
		fprintf(stderr, "FAIL: capabilities did not read CAP exactly\n");
		return 1;
	}

	ret = q3n_hw_read_status(&q3n, &nand_status);
	if (ret || nand_status != (Q3N_NAND_STATUS_READY |
				   Q3N_NAND_STATUS_WP)) {
		fprintf(stderr,
			"FAIL: writable NAND status is %#x, expected %#x\n",
			nand_status,
			Q3N_NAND_STATUS_READY | Q3N_NAND_STATUS_WP);
		return 1;
	}

	fake.status = Q3N_STATUS_READY | Q3N_STATUS_ERROR;
	ret = q3n_hw_reset(&q3n);
	if (ret != -5) {
		fprintf(stderr, "FAIL: controller error was not propagated: %d\n",
			ret);
		return 1;
	}

	printf("ok: Q3N hardware register sequencing verified\n");
	return 0;
}
