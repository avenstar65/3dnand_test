#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_hw_multiplane.h"
#include "qemu_3dnand_priv.h"
#include "qemu_3dnand_regs.h"

#define Q3N_MP_MAIN_SIZE (Q3N_PAGE_SIZE * Q3N_MP_PLANES)
#define Q3N_MP_OOB_SIZE (Q3N_LOGICAL_OOB_SIZE * Q3N_MP_PLANES)
#define Q3N_MP_MAIN_WORDS (Q3N_MP_MAIN_SIZE / sizeof(uint32_t))
#define Q3N_MP_OOB_WORDS (Q3N_MP_OOB_SIZE / sizeof(uint32_t))
#define Q3N_EVENT_MAX (Q3N_MP_MAIN_WORDS + 64)

struct mmio_event {
	uint32_t reg;
	uint32_t value;
};

struct fake_mmio {
	struct mmio_event reads[Q3N_EVENT_MAX];
	struct mmio_event writes[Q3N_EVENT_MAX];
	size_t nreads;
	size_t nwrites;
	uint32_t status;
	uint32_t done_mask;
	uint32_t fail_mask;
	uint32_t ecc_select;
	struct q3n_ecc_result ecc[Q3N_MP_PLANES];
	uint32_t data_words[Q3N_MP_MAIN_WORDS];
	size_t data_read_pos;
	size_t data_write_count;
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: %s\n", message);
	exit(1);
}

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
	if (got != want) {
		fprintf(stderr, "FAIL: %s: got %#x, want %#x\n", name, got, want);
		exit(1);
	}
}

static void expect_int(const char *name, int got, int want)
{
	if (got != want) {
		fprintf(stderr, "FAIL: %s: got %d, want %d\n", name, got, want);
		exit(1);
	}
}

static void log_event(struct mmio_event *events, size_t *count,
		      uint32_t reg, uint32_t value)
{
	if (*count >= Q3N_EVENT_MAX)
		fail("MMIO event log overflow");
	events[*count].reg = reg;
	events[*count].value = value;
	(*count)++;
}

static uint32_t fake_read(void *context, uint32_t reg)
{
	struct fake_mmio *fake = context;
	uint32_t value;

	switch (reg) {
	case Q3N_REG_STATUS:
		value = fake->status;
		break;
	case Q3N_REG_MP_DONE_MASK:
		value = fake->done_mask;
		break;
	case Q3N_REG_MP_FAIL_MASK:
		value = fake->fail_mask;
		break;
	case Q3N_REG_MP_ECC_STATUS:
		value = fake->ecc[fake->ecc_select].status;
		break;
	case Q3N_REG_MP_ECC_MAX_BITFLIPS:
		value = fake->ecc[fake->ecc_select].max_bitflips;
		break;
	case Q3N_REG_MP_ECC_CORRECTED_BITS:
		value = fake->ecc[fake->ecc_select].corrected_bits;
		break;
	case Q3N_REG_MP_ECC_FAILED_STEP:
		value = fake->ecc[fake->ecc_select].failed_step;
		break;
	case Q3N_REG_DATA:
		if (fake->data_read_pos >= Q3N_MP_MAIN_WORDS)
			fail("unexpected DATA read");
		value = fake->data_words[fake->data_read_pos++];
		break;
	default:
		value = 0;
		break;
	}

	log_event(fake->reads, &fake->nreads, reg, value);
	return value;
}

static void fake_write(void *context, uint32_t reg, uint32_t value)
{
	struct fake_mmio *fake = context;

	log_event(fake->writes, &fake->nwrites, reg, value);
	if (reg == Q3N_REG_MP_ECC_SELECT)
		fake->ecc_select = value;
	if (reg == Q3N_REG_DATA)
		fake->data_write_count++;
}

static struct q3n make_q3n(struct fake_mmio *fake)
{
	return (struct q3n) {
		.mmio = {
			.read = fake_read,
			.write = fake_write,
			.context = fake,
		},
	};
}

static struct q3n_mp_addr sample_addr(void)
{
	return (struct q3n_mp_addr) {
		.die = 1,
		.block_in_plane = 7,
		.page_in_block = 19,
	};
}

static void expect_write(const struct fake_mmio *fake, size_t index,
			 uint32_t reg, uint32_t value)
{
	if (index >= fake->nwrites || fake->writes[index].reg != reg ||
	    fake->writes[index].value != value) {
		fprintf(stderr, "FAIL: write %zu: got (%#x,%#x), want (%#x,%#x)\n",
			index, index < fake->nwrites ? fake->writes[index].reg : 0,
			index < fake->nwrites ? fake->writes[index].value : 0,
			reg, value);
		exit(1);
	}
}

static void expect_read(const struct fake_mmio *fake, size_t index,
			uint32_t reg)
{
	if (index >= fake->nreads || fake->reads[index].reg != reg) {
		fprintf(stderr, "FAIL: read %zu: got %#x, want %#x\n", index,
			index < fake->nreads ? fake->reads[index].reg : 0, reg);
		exit(1);
	}
}

static void init_success(struct fake_mmio *fake)
{
	size_t word;

	memset(fake, 0, sizeof(*fake));
	fake->status = Q3N_STATUS_READY;
	fake->done_mask = 0x0f;
	for (word = 0; word < Q3N_MP_MAIN_WORDS; word++)
		fake->data_words[word] = 0x01010101U *
			(word / (Q3N_PAGE_SIZE / sizeof(uint32_t)) + 1);
}

static void expect_result_masks(const struct q3n_mp_result *result,
				uint8_t done, uint8_t fail)
{
	expect_u32("done mask", result->done_mask, done);
	expect_u32("fail mask", result->fail_mask, fail);
}

static void test_read_sequence_ecc_and_staging(void)
{
	struct fake_mmio fake;
	struct q3n q3n;
	struct q3n_mp_result result;
	struct q3n_mp_addr addr = sample_addr();
	uint8_t data[Q3N_MP_MAIN_SIZE];
	uint32_t plane;
	int ret;

	init_success(&fake);
	fake.status |= Q3N_STATUS_ECC_UNCORRECTABLE;
	for (plane = 0; plane < Q3N_MP_PLANES; plane++) {
		fake.ecc[plane].status = plane == 2 ? Q3N_ECC_STATUS_UNCORRECTABLE :
			Q3N_ECC_STATUS_CORRECTED;
		fake.ecc[plane].max_bitflips = plane + 10;
		fake.ecc[plane].corrected_bits = plane + 20;
		fake.ecc[plane].failed_step = plane + 30;
	}
	q3n = make_q3n(&fake);
	memset(data, 0, sizeof(data));
	ret = q3n_hw_mp_read_page(&q3n, &addr, data, false, &result);
	expect_int("uncorrectable ECC transport result", ret, 0);
	expect_result_masks(&result, 0x0f, 0);
	expect_u32("selected plane ECC status", result.plane[2].ecc.status,
		   Q3N_ECC_STATUS_UNCORRECTABLE);

	expect_write(&fake, 0, Q3N_REG_MP_DIE, 1);
	expect_write(&fake, 1, Q3N_REG_MP_BLOCK, 7);
	expect_write(&fake, 2, Q3N_REG_MP_PAGE, 19);
	expect_write(&fake, 3, Q3N_REG_READ_FLAGS, 0);
	expect_write(&fake, 4, Q3N_REG_LEN, Q3N_MP_MAIN_SIZE);
	expect_write(&fake, 5, Q3N_REG_CMD, Q3N_CMD_MP_READ_PAGE);
	expect_read(&fake, 0, Q3N_REG_STATUS);
	expect_read(&fake, 1, Q3N_REG_MP_DONE_MASK);
	expect_read(&fake, 2, Q3N_REG_MP_FAIL_MASK);
	for (plane = 0; plane < Q3N_MP_PLANES; plane++) {
		expect_write(&fake, 6 + plane, Q3N_REG_MP_ECC_SELECT, plane);
		expect_read(&fake, 3 + plane * 4, Q3N_REG_MP_ECC_STATUS);
		expect_read(&fake, 4 + plane * 4, Q3N_REG_MP_ECC_MAX_BITFLIPS);
		expect_read(&fake, 5 + plane * 4, Q3N_REG_MP_ECC_CORRECTED_BITS);
		expect_read(&fake, 6 + plane * 4, Q3N_REG_MP_ECC_FAILED_STEP);
	}
	expect_read(&fake, 19, Q3N_REG_DATA);
	expect_u32("main read words", fake.data_read_pos, Q3N_MP_MAIN_WORDS);
	for (plane = 0; plane < Q3N_MP_PLANES; plane++) {
		if (data[plane * Q3N_PAGE_SIZE] != plane + 1 ||
		    data[(plane + 1) * Q3N_PAGE_SIZE - 1] != plane + 1)
			fail("plane order in main staging buffer is wrong");
	}
}

static void test_program_and_oob_transfers(void)
{
	struct fake_mmio fake;
	struct q3n q3n;
	struct q3n_mp_result result;
	struct q3n_mp_addr addr = sample_addr();
	uint8_t main[Q3N_MP_MAIN_SIZE];
	uint8_t oob[Q3N_MP_OOB_SIZE];
	int ret;

	memset(main, 0xff, sizeof(main));
	memset(oob, 0xff, sizeof(oob));
	init_success(&fake);
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_program_page(&q3n, &addr, main, &result);
	expect_int("main program result", ret, 0);
	expect_u32("main program words", fake.data_write_count, Q3N_MP_MAIN_WORDS);
	expect_write(&fake, 3, Q3N_REG_LEN, Q3N_MP_MAIN_SIZE);
	expect_write(&fake, fake.nwrites - 1, Q3N_REG_CMD,
		     Q3N_CMD_MP_PROGRAM_PAGE);
	if (fake.writes[4].value != 0xffffffffU)
		fail("all-ff main data was elided");

	init_success(&fake);
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_read_oob(&q3n, &addr, oob, &result);
	expect_int("OOB read result", ret, 0);
	expect_u32("OOB read words", fake.data_read_pos, Q3N_MP_OOB_WORDS);
	expect_write(&fake, 3, Q3N_REG_OOB_LEN, Q3N_MP_OOB_SIZE);
	expect_write(&fake, 4, Q3N_REG_CMD, Q3N_CMD_MP_READ_OOB);

	init_success(&fake);
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_program_oob(&q3n, &addr, oob, &result);
	expect_int("OOB program result", ret, 0);
	expect_u32("OOB program words", fake.data_write_count, Q3N_MP_OOB_WORDS);
	expect_write(&fake, 3, Q3N_REG_OOB_LEN, Q3N_MP_OOB_SIZE);
	expect_write(&fake, fake.nwrites - 1, Q3N_REG_CMD,
		     Q3N_CMD_MP_PROGRAM_OOB);
}

static void test_erase_and_transport_masks(void)
{
	struct fake_mmio fake;
	struct q3n q3n;
	struct q3n_mp_result result;
	struct q3n_mp_addr addr = sample_addr();
	int ret;

	init_success(&fake);
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_erase_group(&q3n, &addr, &result);
	expect_int("erase result", ret, 0);
	expect_write(&fake, 3, Q3N_REG_CMD, Q3N_CMD_MP_ERASE_GROUP);
	expect_u32("erase data writes", fake.data_write_count, 0);
	expect_u32("erase data reads", fake.data_read_pos, 0);

	init_success(&fake);
	fake.status |= Q3N_STATUS_ERROR;
	fake.done_mask = 0x0d;
	fake.fail_mask = 0x02;
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_erase_group(&q3n, &addr, &result);
	expect_int("partial group failure", ret, -EIO);
	expect_result_masks(&result, 0x0d, 0x02);
	expect_int("failed plane status", result.plane[1].status, -EIO);

	init_success(&fake);
	fake.done_mask = 0x0f;
	fake.fail_mask = 0x01;
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_erase_group(&q3n, &addr, &result);
	expect_int("overlapping masks", ret, -EPROTO);
	expect_result_masks(&result, 0x0f, 0x01);

	init_success(&fake);
	fake.done_mask = 0x0e;
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_erase_group(&q3n, &addr, &result);
	expect_int("incomplete masks", ret, -EPROTO);
	expect_result_masks(&result, 0x0e, 0);
}

static void test_partial_read_keeps_ecc_and_staging(void)
{
	struct fake_mmio fake;
	struct q3n q3n;
	struct q3n_mp_result result;
	struct q3n_mp_addr addr = sample_addr();
	uint8_t data[Q3N_MP_MAIN_SIZE];
	int ret;

	init_success(&fake);
	fake.status |= Q3N_STATUS_ERROR;
	fake.done_mask = 0x0d;
	fake.fail_mask = 0x02;
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_read_page(&q3n, &addr, data, false, &result);
	expect_int("partial read result", ret, -EIO);
	expect_result_masks(&result, 0x0d, 0x02);
	expect_int("partial read failed plane status", result.plane[1].status,
		   -EIO);
	expect_u32("partial read words", fake.data_read_pos, Q3N_MP_MAIN_WORDS);
	expect_write(&fake, 9, Q3N_REG_MP_ECC_SELECT, 3);
	expect_read(&fake, 19, Q3N_REG_DATA);
}

static void test_timeout_and_prevalidation_failures(void)
{
	struct fake_mmio fake;
	struct q3n q3n;
	struct q3n_mp_result result;
	struct q3n_mp_addr addr = sample_addr();
	int ret;

	init_success(&fake);
	fake.status = 0;
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_erase_group(&q3n, &addr, &result);
	expect_int("ready timeout", ret, -ETIMEDOUT);
	expect_result_masks(&result, 0, 0);
	if (fake.nreads != 1000)
		fail("timeout did not poll exactly 1000 times");

	init_success(&fake);
	fake.done_mask = 0;
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_erase_group(&q3n, &addr, &result);
	expect_int("prevalidation without ERROR", ret, -EINVAL);
	expect_result_masks(&result, 0, 0);

	init_success(&fake);
	fake.status |= Q3N_STATUS_ERROR;
	fake.done_mask = 0;
	q3n = make_q3n(&fake);
	ret = q3n_hw_mp_erase_group(&q3n, &addr, &result);
	expect_int("prevalidation with ERROR", ret, -EIO);
	expect_result_masks(&result, 0, 0);
}

int main(void)
{
	test_read_sequence_ecc_and_staging();
	test_program_and_oob_transfers();
	test_erase_and_transport_masks();
	test_partial_read_keeps_ecc_and_staging();
	test_timeout_and_prevalidation_failures();
	printf("ok: Q3N multi-plane MMIO/PIO sequencing verified\n");
	return 0;
}
