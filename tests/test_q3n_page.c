#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_page.h"
#include "qemu_3dnand_regs.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define MAX_CALLS 16
#define MAX_FIXTURES 8
#define MAX_PROGRAMS 5

enum fake_op {
	FAKE_READ_PAGE,
	FAKE_PROGRAM_PAGE,
	FAKE_READ_OOB,
	FAKE_PROGRAM_OOB,
	FAKE_ERASE_BLOCK,
};

struct fake_call {
	enum fake_op op;
	uint32_t address;
	bool raw;
	bool has_oob;
};

struct read_fixture {
	uint32_t page;
	int ret;
	uint8_t data_byte;
	uint8_t oob_byte;
	struct q3n_ecc_result ecc;
};

struct fake_hw {
	struct fake_call calls[MAX_CALLS];
	size_t call_count;
	struct read_fixture fixtures[MAX_FIXTURES];
	size_t fixture_count;
	uint32_t fail_program_page;
	int fail_program_ret;
	uint8_t program_data[MAX_PROGRAMS][Q3N_PAGE_SIZE];
	uint8_t program_oob[MAX_PROGRAMS][Q3N_LOGICAL_OOB_SIZE];
	bool program_has_oob[MAX_PROGRAMS];
};

static struct fake_hw fake;

static void fail_value(const char *name, uint64_t got, uint64_t want)
{
	fprintf(stderr, "FAIL: %s: got %llu, want %llu\n", name,
		(unsigned long long)got, (unsigned long long)want);
	exit(1);
}

static void expect_u64(const char *name, uint64_t got, uint64_t want)
{
	if (got != want)
		fail_value(name, got, want);
}

static void expect_mem(const char *name, const void *got, const void *want,
		       size_t length)
{
	if (memcmp(got, want, length)) {
		fprintf(stderr, "FAIL: %s: buffers differ\n", name);
		exit(1);
	}
}

static void expect_fill(const char *name, const uint8_t *buffer, size_t length,
			uint8_t value)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (buffer[i] != value)
			fail_value(name, buffer[i], value);
	}
}

static void reset_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.fail_program_page = UINT32_MAX;
}

static void log_call(enum fake_op op, uint32_t address, bool raw, bool has_oob)
{
	if (fake.call_count == ARRAY_SIZE(fake.calls)) {
		fprintf(stderr, "FAIL: fake call log overflow\n");
		exit(1);
	}

	fake.calls[fake.call_count++] = (struct fake_call) {
		.op = op,
		.address = address,
		.raw = raw,
		.has_oob = has_oob,
	};
}

static void expect_call(size_t index, enum fake_op op, uint32_t address,
			bool raw, bool has_oob)
{
	const struct fake_call *call;

	if (index >= fake.call_count)
		fail_value("missing fake call", fake.call_count, index + 1);
	call = &fake.calls[index];
	if (call->op != op || call->address != address ||
	    call->raw != raw || call->has_oob != has_oob) {
		fprintf(stderr,
			"FAIL: call %zu got {op=%d,address=%u,raw=%d,oob=%d}, "
			"want {op=%d,address=%u,raw=%d,oob=%d}\n",
			index, call->op, call->address, call->raw, call->has_oob,
			op, address, raw, has_oob);
		exit(1);
	}
}

static struct read_fixture *find_fixture(uint32_t page)
{
	size_t i;

	for (i = 0; i < fake.fixture_count; i++) {
		if (fake.fixtures[i].page == page)
			return &fake.fixtures[i];
	}

	fprintf(stderr, "FAIL: no read fixture for physical page %u\n", page);
	exit(1);
}

static void add_fixture(uint32_t page, uint8_t data_byte, uint8_t oob_byte,
			uint32_t status, uint32_t max_bitflips,
			uint32_t corrected_bits, int ret)
{
	struct read_fixture *fixture;

	if (fake.fixture_count == ARRAY_SIZE(fake.fixtures)) {
		fprintf(stderr, "FAIL: read fixture overflow\n");
		exit(1);
	}

	fixture = &fake.fixtures[fake.fixture_count++];
	*fixture = (struct read_fixture) {
		.page = page,
		.ret = ret,
		.data_byte = data_byte,
		.oob_byte = oob_byte,
		.ecc = {
			.status = status,
			.max_bitflips = max_bitflips,
			.corrected_bits = corrected_bits,
			.failed_step = status & Q3N_ECC_STATUS_UNCORRECTABLE ?
				2 : Q3N_ECC_NO_FAILED_STEP,
		},
	};
}

int q3n_hw_read_page(struct q3n *q3n, uint32_t page, void *data, void *oob,
		     bool raw, struct q3n_ecc_result *result)
{
	struct read_fixture *fixture = find_fixture(page);

	(void)q3n;
	log_call(FAKE_READ_PAGE, page, raw, oob != NULL);
	if (fixture->ret)
		return fixture->ret;

	memset(data, fixture->data_byte, Q3N_PAGE_SIZE);
	if (oob)
		memset(oob, fixture->oob_byte, Q3N_LOGICAL_OOB_SIZE);
	if (!raw && result)
		*result = fixture->ecc;
	return 0;
}

int q3n_hw_program_page(struct q3n *q3n, uint32_t page,
			const void *data, const void *oob)
{
	size_t index = fake.call_count;

	(void)q3n;
	log_call(FAKE_PROGRAM_PAGE, page, false, oob != NULL);
	if (index >= MAX_PROGRAMS) {
		fprintf(stderr, "FAIL: program fixture overflow\n");
		exit(1);
	}
	memcpy(fake.program_data[index], data, Q3N_PAGE_SIZE);
	fake.program_has_oob[index] = oob != NULL;
	if (oob)
		memcpy(fake.program_oob[index], oob, Q3N_LOGICAL_OOB_SIZE);
	if (page == fake.fail_program_page)
		return fake.fail_program_ret;
	return 0;
}

int q3n_hw_read_oob(struct q3n *q3n, uint32_t page, void *oob)
{
	struct read_fixture *fixture = find_fixture(page);

	(void)q3n;
	log_call(FAKE_READ_OOB, page, false, true);
	if (fixture->ret)
		return fixture->ret;
	memset(oob, fixture->oob_byte, Q3N_LOGICAL_OOB_SIZE);
	return 0;
}

int q3n_hw_program_oob(struct q3n *q3n, uint32_t page, const void *oob)
{
	size_t index = fake.call_count;

	(void)q3n;
	log_call(FAKE_PROGRAM_OOB, page, false, true);
	if (index >= MAX_PROGRAMS) {
		fprintf(stderr, "FAIL: OOB program fixture overflow\n");
		exit(1);
	}
	memcpy(fake.program_oob[index], oob, Q3N_LOGICAL_OOB_SIZE);
	return 0;
}

int q3n_hw_erase_block(struct q3n *q3n, uint32_t block)
{
	(void)q3n;
	log_call(FAKE_ERASE_BLOCK, block, false, false);
	return 0;
}

static struct q3n make_q3n(bool raid_enabled)
{
	struct q3n q3n = {
		.physical_geometry = {
			.writesize = Q3N_PAGE_SIZE,
			.oobsize = Q3N_LOGICAL_OOB_SIZE,
			.pages_per_block = Q3N_PAGES_PER_BLOCK,
			.blocks = Q3N_DATA_BLOCKS,
		},
	};
	int ret;

	ret = q3n_page_layer_init(&q3n, raid_enabled, 4);
	expect_u64("page layer init", ret, 0);
	return q3n;
}

static void test_identity_path(void)
{
	struct q3n q3n;
	struct q3n_page_result result;
	uint8_t data[Q3N_PAGE_SIZE];
	uint8_t expected_data[Q3N_PAGE_SIZE];
	uint8_t oob[Q3N_LOGICAL_OOB_SIZE];
	uint8_t expected_oob[Q3N_LOGICAL_OOB_SIZE];
	int ret;

	reset_fake();
	q3n = make_q3n(false);
	expect_u64("identity writesize", q3n.geometry.writesize, Q3N_PAGE_SIZE);
	expect_u64("identity OOB size", q3n.geometry.oobsize,
		   Q3N_LOGICAL_OOB_SIZE);
	expect_u64("identity parity scratch absent",
		   (uintptr_t)q3n.parity_scratch, 0);

	add_fixture(17, 0x5a, 0xa5, Q3N_ECC_STATUS_CORRECTED, 9, 13, 0);
	memset(&result, 0xcc, sizeof(result));
	ret = q3n_page_read(&q3n, 17, data, oob, false, &result);
	expect_u64("identity read return", ret, 0);
	expect_u64("identity read calls", fake.call_count, 1);
	expect_call(0, FAKE_READ_PAGE, 17, false, true);
	memset(expected_data, 0x5a, sizeof(expected_data));
	memset(expected_oob, 0xa5, sizeof(expected_oob));
	expect_mem("identity read data", data, expected_data, sizeof(data));
	expect_mem("identity read OOB", oob, expected_oob, sizeof(oob));
	expect_u64("identity max bitflips", result.max_bitflips, 9);
	expect_u64("identity corrected bits", result.corrected_bits, 13);
	expect_u64("identity failed pages", result.failed_data_pages, 0);
	expect_u64("identity parity failure", result.parity_failed, false);
	expect_u64("identity recovered", result.recovered, false);

	reset_fake();
	memset(data, 0x3c, sizeof(data));
	memset(oob, 0xc3, sizeof(oob));
	ret = q3n_page_write(&q3n, 23, data, oob);
	expect_u64("identity write return", ret, 0);
	expect_u64("identity write calls", fake.call_count, 1);
	expect_call(0, FAKE_PROGRAM_PAGE, 23, false, true);
	expect_mem("identity write data", fake.program_data[0], data,
		   sizeof(data));
	expect_mem("identity write OOB", fake.program_oob[0], oob, sizeof(oob));

	reset_fake();
	add_fixture(29, 0, 0x69, 0, 0, 0, 0);
	ret = q3n_page_read_oob(&q3n, 29, oob);
	expect_u64("identity OOB read return", ret, 0);
	expect_u64("identity OOB read calls", fake.call_count, 1);
	expect_call(0, FAKE_READ_OOB, 29, false, true);
	expect_fill("identity OOB read buffer", oob, sizeof(oob), 0x69);

	reset_fake();
	memset(oob, 0x96, sizeof(oob));
	ret = q3n_page_write_oob(&q3n, 31, oob);
	expect_u64("identity OOB write return", ret, 0);
	expect_u64("identity OOB write calls", fake.call_count, 1);
	expect_call(0, FAKE_PROGRAM_OOB, 31, false, true);
	expect_mem("identity OOB write buffer", fake.program_oob[0], oob,
		   sizeof(oob));

	reset_fake();
	ret = q3n_page_erase_block(&q3n, 7);
	expect_u64("identity erase return", ret, 0);
	expect_u64("identity erase calls", fake.call_count, 1);
	expect_call(0, FAKE_ERASE_BLOCK, 7, false, false);

	q3n_page_layer_cleanup(&q3n);
	expect_u64("identity cleanup clears ops", (uintptr_t)q3n.page_ops, 0);
}

static void test_identity_erased_program_skips(void)
{
	struct q3n q3n;
	uint8_t data[Q3N_PAGE_SIZE];
	uint8_t oob[Q3N_LOGICAL_OOB_SIZE];
	int ret;

	reset_fake();
	q3n = make_q3n(false);
	memset(data, 0xff, sizeof(data));
	ret = q3n_page_write(&q3n, 0, data, NULL);
	expect_u64("identity erased data without OOB return", ret, 0);
	expect_u64("identity erased data without OOB skips program",
		   fake.call_count, 0);

	reset_fake();
	memset(oob, 0xff, sizeof(oob));
	ret = q3n_page_write(&q3n, 0, data, oob);
	expect_u64("identity fully erased page return", ret, 0);
	expect_u64("identity fully erased page skips program", fake.call_count, 0);

	reset_fake();
	oob[Q3N_LOGICAL_OOB_SIZE / 2] = 0x7e;
	ret = q3n_page_write(&q3n, 0, data, oob);
	expect_u64("identity erased main with live OOB return", ret, 0);
	expect_u64("identity erased main uses one OOB program",
		   fake.call_count, 1);
	expect_call(0, FAKE_PROGRAM_OOB, 0, false, true);
	expect_mem("identity OOB-only bytes", fake.program_oob[0], oob,
		   sizeof(oob));

	reset_fake();
	memset(data, 0xff, sizeof(data));
	data[Q3N_PAGE_SIZE / 2] = 0x7d;
	ret = q3n_page_write(&q3n, 0, data, NULL);
	expect_u64("identity middle-byte main return", ret, 0);
	expect_u64("identity middle-byte main is programmed", fake.call_count, 1);
	expect_call(0, FAKE_PROGRAM_PAGE, 0, false, false);
	expect_mem("identity middle-byte scan covers full buffer",
		   fake.program_data[0], data, sizeof(data));

	reset_fake();
	memset(oob, 0xff, sizeof(oob));
	ret = q3n_page_write_oob(&q3n, 0, oob);
	expect_u64("identity erased OOB-only return", ret, 0);
	expect_u64("identity erased OOB-only skips program", fake.call_count, 0);

	reset_fake();
	oob[Q3N_LOGICAL_OOB_SIZE / 2] = 0x7c;
	ret = q3n_page_write_oob(&q3n, 0, oob);
	expect_u64("identity middle-byte OOB-only return", ret, 0);
	expect_u64("identity middle-byte OOB-only is programmed",
		   fake.call_count, 1);
	expect_call(0, FAKE_PROGRAM_OOB, 0, false, true);
	q3n_page_layer_cleanup(&q3n);
}

#ifdef Q3N_TEST_RAID
static void fill_logical_data(uint8_t *data)
{
	memset(data + 0 * Q3N_PAGE_SIZE, 0x11, Q3N_PAGE_SIZE);
	memset(data + 1 * Q3N_PAGE_SIZE, 0x22, Q3N_PAGE_SIZE);
	memset(data + 2 * Q3N_PAGE_SIZE, 0x44, Q3N_PAGE_SIZE);
	memset(data + 3 * Q3N_PAGE_SIZE, 0x80, Q3N_PAGE_SIZE);
}

static void fill_logical_oob(uint8_t *oob)
{
	memset(oob + 0 * Q3N_LOGICAL_OOB_SIZE, 0x10,
	       Q3N_LOGICAL_OOB_SIZE);
	memset(oob + 1 * Q3N_LOGICAL_OOB_SIZE, 0x20,
	       Q3N_LOGICAL_OOB_SIZE);
	memset(oob + 2 * Q3N_LOGICAL_OOB_SIZE, 0x40,
	       Q3N_LOGICAL_OOB_SIZE);
	memset(oob + 3 * Q3N_LOGICAL_OOB_SIZE, 0x80,
	       Q3N_LOGICAL_OOB_SIZE);
}

static void add_clean_stripe(void)
{
	add_fixture(0, 0x11, 0x10, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(1, 0x22, 0x20, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(2, 0x44, 0x40, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(3, 0x80, 0x80, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
}

static void test_raid_write_order_parity_and_oob(void)
{
	struct q3n q3n;
	uint8_t data[4 * Q3N_PAGE_SIZE];
	uint8_t oob[4 * Q3N_LOGICAL_OOB_SIZE];
	size_t i;
	int ret;

	reset_fake();
	q3n = make_q3n(true);
	fill_logical_data(data);
	fill_logical_oob(oob);

	ret = q3n_page_write(&q3n, 0, data, oob);
	expect_u64("RAID write return", ret, 0);
	expect_u64("RAID write call count", fake.call_count, 5);
	for (i = 0; i < 4; i++) {
		expect_call(i, FAKE_PROGRAM_PAGE, i, false, true);
		expect_fill("RAID data slice", fake.program_data[i],
			    Q3N_PAGE_SIZE, data[i * Q3N_PAGE_SIZE]);
		expect_mem("RAID OOB slice", fake.program_oob[i],
			   oob + i * Q3N_LOGICAL_OOB_SIZE,
			   Q3N_LOGICAL_OOB_SIZE);
	}
	expect_call(4, FAKE_PROGRAM_PAGE, 4, false, false);
	expect_u64("parity OOB must be NULL", fake.program_has_oob[4], false);
	expect_fill("four-slice XOR parity", fake.program_data[4],
		    Q3N_PAGE_SIZE, 0xf7);
	q3n_page_layer_cleanup(&q3n);
}

static void test_raid_write_stops_at_each_failure(void)
{
	const uint32_t failure_pages[] = { 0, 2, 3, 4 };
	struct q3n q3n;
	uint8_t data[4 * Q3N_PAGE_SIZE];
	uint8_t oob[4 * Q3N_LOGICAL_OOB_SIZE];
	size_t case_index;

	q3n = make_q3n(true);
	fill_logical_data(data);
	fill_logical_oob(oob);

	for (case_index = 0; case_index < ARRAY_SIZE(failure_pages);
	     case_index++) {
		int ret;

		reset_fake();
		fake.fail_program_page = failure_pages[case_index];
		fake.fail_program_ret = -EIO;
		ret = q3n_page_write(&q3n, 0, data, oob);
		expect_u64("RAID write failure propagated", ret, (uint64_t)-EIO);
		expect_u64("RAID write stops at first failure", fake.call_count,
			   failure_pages[case_index] + 1);
	}
	q3n_page_layer_cleanup(&q3n);
}

static void test_raid_erased_program_skips(void)
{
	struct q3n q3n;
	uint8_t data[4 * Q3N_PAGE_SIZE];
	uint8_t oob[4 * Q3N_LOGICAL_OOB_SIZE];
	size_t middle = Q3N_PAGE_SIZE / 2;
	int ret;

	q3n = make_q3n(true);

	reset_fake();
	fill_logical_data(data);
	memset(data + Q3N_PAGE_SIZE, 0xff, Q3N_PAGE_SIZE);
	memset(oob, 0xff, sizeof(oob));
	ret = q3n_page_write(&q3n, 0, data, oob);
	expect_u64("RAID erased data/all-ff OOB return", ret, 0);
	expect_u64("RAID erased slice is omitted", fake.call_count, 4);
	expect_call(0, FAKE_PROGRAM_PAGE, 0, false, true);
	expect_call(1, FAKE_PROGRAM_PAGE, 2, false, true);
	expect_call(2, FAKE_PROGRAM_PAGE, 3, false, true);
	expect_call(3, FAKE_PROGRAM_PAGE, 4, false, false);
	expect_fill("RAID parity includes omitted erased slice",
		    fake.program_data[3], Q3N_PAGE_SIZE, 0x2a);

	reset_fake();
	fill_logical_data(data);
	memset(data + Q3N_PAGE_SIZE, 0xff, Q3N_PAGE_SIZE);
	memset(oob, 0xff, sizeof(oob));
	oob[Q3N_LOGICAL_OOB_SIZE + Q3N_LOGICAL_OOB_SIZE / 2] = 0x6d;
	ret = q3n_page_write(&q3n, 0, data, oob);
	expect_u64("RAID erased main/live OOB return", ret, 0);
	expect_u64("RAID erased main/live OOB call count", fake.call_count, 5);
	expect_call(0, FAKE_PROGRAM_PAGE, 0, false, true);
	expect_call(1, FAKE_PROGRAM_OOB, 1, false, true);
	expect_call(2, FAKE_PROGRAM_PAGE, 2, false, true);
	expect_call(3, FAKE_PROGRAM_PAGE, 3, false, true);
	expect_call(4, FAKE_PROGRAM_PAGE, 4, false, false);
	expect_mem("RAID OOB-only slice", fake.program_oob[1],
		   oob + Q3N_LOGICAL_OOB_SIZE, Q3N_LOGICAL_OOB_SIZE);

	reset_fake();
	memset(data + 0 * Q3N_PAGE_SIZE, 0x11, Q3N_PAGE_SIZE);
	memset(data + 1 * Q3N_PAGE_SIZE, 0x22, Q3N_PAGE_SIZE);
	memset(data + 2 * Q3N_PAGE_SIZE, 0x44, Q3N_PAGE_SIZE);
	memset(data + 3 * Q3N_PAGE_SIZE, 0x88, Q3N_PAGE_SIZE);
	ret = q3n_page_write(&q3n, 0, data, NULL);
	expect_u64("all-ff parity write return", ret, 0);
	expect_u64("all-ff computed parity is omitted", fake.call_count, 4);
	expect_call(3, FAKE_PROGRAM_PAGE, 3, false, false);

	reset_fake();
	data[3 * Q3N_PAGE_SIZE + middle] = 0x87;
	ret = q3n_page_write(&q3n, 0, data, NULL);
	expect_u64("middle-byte parity write return", ret, 0);
	expect_u64("non-ff middle parity remains last", fake.call_count, 5);
	expect_call(4, FAKE_PROGRAM_PAGE, 4, false, false);
	expect_fill("parity prefix remains erased", fake.program_data[4],
		    middle, 0xff);
	expect_u64("parity middle byte detected",
		   fake.program_data[4][middle], 0xf0);
	expect_fill("parity suffix remains erased",
		    fake.program_data[4] + middle + 1,
		    Q3N_PAGE_SIZE - middle - 1, 0xff);

	reset_fake();
	memset(oob, 0xff, sizeof(oob));
	ret = q3n_page_write_oob(&q3n, 0, oob);
	expect_u64("RAID erased OOB-only return", ret, 0);
	expect_u64("RAID erased OOB-only chunks skip program",
		   fake.call_count, 0);

	reset_fake();
	oob[2 * Q3N_LOGICAL_OOB_SIZE + Q3N_LOGICAL_OOB_SIZE / 2] = 0x5c;
	ret = q3n_page_write_oob(&q3n, 0, oob);
	expect_u64("RAID middle-byte OOB-only return", ret, 0);
	expect_u64("RAID only live OOB chunk is programmed",
		   fake.call_count, 1);
	expect_call(0, FAKE_PROGRAM_OOB, 2, false, true);
	q3n_page_layer_cleanup(&q3n);
}

static void test_raid_oob_and_erase_mapping(void)
{
	struct q3n q3n;
	uint8_t oob[4 * Q3N_LOGICAL_OOB_SIZE];
	size_t i;
	int ret;

	reset_fake();
	q3n = make_q3n(true);
	add_clean_stripe();
	ret = q3n_page_read_oob(&q3n, 0, oob);
	expect_u64("RAID OOB read return", ret, 0);
	expect_u64("RAID OOB read calls", fake.call_count, 4);
	for (i = 0; i < 4; i++) {
		expect_call(i, FAKE_READ_OOB, i, false, true);
		expect_fill("RAID OOB read slice",
			    oob + i * Q3N_LOGICAL_OOB_SIZE,
			    Q3N_LOGICAL_OOB_SIZE, (uint8_t)(0x10U << i));
	}

	reset_fake();
	fill_logical_oob(oob);
	ret = q3n_page_write_oob(&q3n, 0, oob);
	expect_u64("RAID OOB write return", ret, 0);
	expect_u64("RAID OOB write calls", fake.call_count, 4);
	for (i = 0; i < 4; i++) {
		expect_call(i, FAKE_PROGRAM_OOB, i, false, true);
		expect_mem("RAID OOB write slice", fake.program_oob[i],
			   oob + i * Q3N_LOGICAL_OOB_SIZE,
			   Q3N_LOGICAL_OOB_SIZE);
	}

	reset_fake();
	ret = q3n_page_erase_block(&q3n, 7);
	expect_u64("RAID erase return", ret, 0);
	expect_u64("RAID erase calls", fake.call_count, 1);
	expect_call(0, FAKE_ERASE_BLOCK, 7, false, false);
	q3n_page_layer_cleanup(&q3n);
}

static void test_raid_clean_and_corrected_reads(void)
{
	struct q3n q3n;
	struct q3n_page_result result;
	uint8_t data[4 * Q3N_PAGE_SIZE];
	uint8_t expected[4 * Q3N_PAGE_SIZE];
	uint8_t oob[4 * Q3N_LOGICAL_OOB_SIZE];
	size_t i;
	int ret;

	reset_fake();
	q3n = make_q3n(true);
	add_clean_stripe();
	ret = q3n_page_read(&q3n, 0, data, oob, false, &result);
	expect_u64("clean RAID read return", ret, 0);
	expect_u64("clean RAID read calls", fake.call_count, 4);
	fill_logical_data(expected);
	expect_mem("clean RAID data", data, expected, sizeof(data));
	for (i = 0; i < 4; i++)
		expect_call(i, FAKE_READ_PAGE, i, false, true);
	expect_u64("clean failed pages", result.failed_data_pages, 0);
	expect_u64("clean max bitflips", result.max_bitflips, 0);
	expect_u64("clean corrected bits", result.corrected_bits, 0);
	expect_u64("clean read not recovered", result.recovered, false);

	reset_fake();
	add_fixture(0, 0x11, 0x10, Q3N_ECC_STATUS_CORRECTED, 3, 3, 0);
	add_fixture(1, 0x22, 0x20, Q3N_ECC_STATUS_CORRECTED, 12, 20, 0);
	add_fixture(2, 0x44, 0x40, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(3, 0x80, 0x80, Q3N_ECC_STATUS_CORRECTED, 7, 11, 0);
	ret = q3n_page_read(&q3n, 0, data, NULL, false, &result);
	expect_u64("corrected RAID read return", ret, 0);
	expect_u64("corrected RAID read calls", fake.call_count, 4);
	expect_u64("aggregate max bitflips", result.max_bitflips, 12);
	expect_u64("aggregate corrected bits", result.corrected_bits, 34);
	expect_u64("corrected failed pages", result.failed_data_pages, 0);
	q3n_page_layer_cleanup(&q3n);
}

static void test_raid_retry_and_recovery_gates(void)
{
	struct q3n q3n;
	struct q3n_page_result result;
	uint8_t data[4 * Q3N_PAGE_SIZE];
	int ret;

	reset_fake();
	q3n = make_q3n(true);
	add_fixture(0, 0x11, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(1, 0xee, 0, Q3N_ECC_STATUS_UNCORRECTABLE, 0, 0, 0);
	add_fixture(2, 0x44, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(3, 0x80, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	q3n.retry_mode = 2;
	ret = q3n_page_read(&q3n, 0, data, NULL, false, &result);
	expect_u64("pre-final retry return", ret, 0);
	expect_u64("pre-final retry skips parity", fake.call_count, 4);
	expect_u64("pre-final failed pages", result.failed_data_pages, 1);
	expect_u64("pre-final not recovered", result.recovered, false);

	reset_fake();
	add_fixture(0, 0x11, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(1, 0x22, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(2, 0xee, 0, Q3N_ECC_STATUS_UNCORRECTABLE, 2, 5, 0);
	add_fixture(3, 0x80, 0, Q3N_ECC_STATUS_CORRECTED, 7, 9, 0);
	add_fixture(4, 0xf7, 0, Q3N_ECC_STATUS_CORRECTED, 19, 99, 0);
	q3n.retry_mode = 3;
	ret = q3n_page_read(&q3n, 0, data, NULL, false, &result);
	expect_u64("final retry recovery return", ret, 0);
	expect_u64("final retry reads parity last", fake.call_count, 5);
	expect_call(4, FAKE_READ_PAGE, 4, false, false);
	expect_fill("exact reconstructed 16 KiB literal",
		    data + 2 * Q3N_PAGE_SIZE, Q3N_PAGE_SIZE, 0x44);
	expect_u64("recovery clears failed pages", result.failed_data_pages, 0);
	expect_u64("recovery marks result", result.recovered, true);
	expect_u64("recovery threshold", result.max_bitflips, Q3N_ECC_STRENGTH);
	expect_u64("parity counters excluded", result.corrected_bits, 14);
	expect_u64("recovery counter", q3n.raid_recovered_pages, 1);

	reset_fake();
	add_fixture(0, 0xee, 0, Q3N_ECC_STATUS_UNCORRECTABLE, 0, 0, 0);
	add_fixture(1, 0x22, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(2, 0xdd, 0, Q3N_ECC_STATUS_UNCORRECTABLE, 0, 0, 0);
	add_fixture(3, 0x80, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	ret = q3n_page_read(&q3n, 0, data, NULL, false, &result);
	expect_u64("double failure return", ret, 0);
	expect_u64("double failure skips parity", fake.call_count, 4);
	expect_u64("double failed pages preserved", result.failed_data_pages, 2);
	expect_u64("double failure not recovered", result.recovered, false);
	expect_u64("double failure counter unchanged", q3n.raid_recovered_pages, 1);
	q3n_page_layer_cleanup(&q3n);
}

static void test_raid_parity_and_transport_failures(void)
{
	struct q3n q3n;
	struct q3n_page_result result;
	uint8_t data[4 * Q3N_PAGE_SIZE];
	int ret;

	reset_fake();
	q3n = make_q3n(true);
	q3n.retry_mode = 3;
	add_fixture(0, 0x11, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(1, 0xee, 0, Q3N_ECC_STATUS_UNCORRECTABLE, 0, 0, 0);
	add_fixture(2, 0x44, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(3, 0x80, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(4, 0xf7, 0, Q3N_ECC_STATUS_UNCORRECTABLE, 0, 0, 0);
	ret = q3n_page_read(&q3n, 0, data, NULL, false, &result);
	expect_u64("failed parity return", ret, 0);
	expect_u64("failed parity call count", fake.call_count, 5);
	expect_u64("failed parity flag", result.parity_failed, true);
	expect_u64("failed parity keeps data failure", result.failed_data_pages, 1);
	expect_u64("failed parity not recovered", result.recovered, false);

	reset_fake();
	add_fixture(0, 0x11, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(1, 0, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, -EIO);
	ret = q3n_page_read(&q3n, 0, data, NULL, false, &result);
	expect_u64("data transport error unchanged", ret, (uint64_t)-EIO);
	expect_u64("data transport stops reads", fake.call_count, 2);

	reset_fake();
	add_fixture(0, 0x11, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(1, 0xee, 0, Q3N_ECC_STATUS_UNCORRECTABLE, 0, 0, 0);
	add_fixture(2, 0x44, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(3, 0x80, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	add_fixture(4, 0, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, -ETIMEDOUT);
	ret = q3n_page_read(&q3n, 0, data, NULL, false, &result);
	expect_u64("parity transport error unchanged", ret,
		   (uint64_t)-ETIMEDOUT);
	expect_u64("parity transport call count", fake.call_count, 5);
	q3n_page_layer_cleanup(&q3n);
}

static void test_raid_raw_read_hides_parity_and_raw_write_keeps_it(void)
{
	struct q3n q3n;
	struct q3n_page_result result;
	uint8_t data[4 * Q3N_PAGE_SIZE];
	uint8_t expected[4 * Q3N_PAGE_SIZE];
	size_t i;
	int ret;

	reset_fake();
	q3n = make_q3n(true);
	add_clean_stripe();
	add_fixture(4, 0xf7, 0, Q3N_ECC_STATUS_CLEAN, 0, 0, 0);
	q3n.retry_mode = 3;
	memset(&result, 0xcc, sizeof(result));
	ret = q3n_page_read(&q3n, 0, data, NULL, true, &result);
	expect_u64("raw RAID read return", ret, 0);
	expect_u64("raw RAID read hides parity", fake.call_count, 4);
	for (i = 0; i < 4; i++)
		expect_call(i, FAKE_READ_PAGE, i, true, false);
	fill_logical_data(expected);
	expect_mem("raw RAID concatenation", data, expected, sizeof(data));
	expect_u64("raw RAID never recovered", result.recovered, false);
	expect_u64("raw RAID no failed pages", result.failed_data_pages, 0);

	reset_fake();
	fill_logical_data(data);
	ret = q3n_page_write(&q3n, 0, data, NULL);
	expect_u64("raw-style write return", ret, 0);
	expect_u64("raw-style write includes parity", fake.call_count, 5);
	expect_call(4, FAKE_PROGRAM_PAGE, 4, false, false);
	expect_fill("raw-style write parity", fake.program_data[4],
		    Q3N_PAGE_SIZE, 0xf7);
	q3n_page_layer_cleanup(&q3n);
}
#endif

int main(void)
{
	test_identity_path();
	test_identity_erased_program_skips();
#ifdef Q3N_TEST_RAID
	test_raid_write_order_parity_and_oob();
	test_raid_write_stops_at_each_failure();
	test_raid_erased_program_skips();
	test_raid_oob_and_erase_mapping();
	test_raid_clean_and_corrected_reads();
	test_raid_retry_and_recovery_gates();
	test_raid_parity_and_transport_failures();
	test_raid_raw_read_hides_parity_and_raw_write_keeps_it();
	printf("ok: Q3N identity and synchronous page RAID behavior verified\n");
#else
	printf("ok: Q3N identity page layer links without page RAID\n");
#endif
	return 0;
}
