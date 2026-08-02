#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu_3dnand_hw_multiplane.h"
#include "qemu_3dnand_page.h"
#include "qemu_3dnand_regs.h"

#define MP_MAIN_SIZE (4U * 16384U)
#define MP_OOB_SIZE (4U * 1024U)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#ifdef Q3N_TEST_ALLOC_FAIL
#define Q3N_TEST_UNUSED __attribute__((unused))
#else
#define Q3N_TEST_UNUSED
#endif

enum fake_op {
	FAKE_READ_PAGE,
	FAKE_PROGRAM_PAGE,
	FAKE_READ_OOB,
	FAKE_PROGRAM_OOB,
	FAKE_ERASE,
};

struct fake_call {
	enum fake_op op;
	struct q3n_mp_addr addr;
	bool raw;
};

struct fake_hw {
	struct fake_call calls[16];
	size_t calls_count;
	int main_ret;
	int oob_ret;
	int erase_ret;
	struct q3n_mp_result read_result;
	struct q3n_mp_result main_result;
	uint8_t read_oob[MP_OOB_SIZE];
	uint8_t last_oob[MP_OOB_SIZE];
	uint8_t last_main[MP_MAIN_SIZE];
};

static struct fake_hw fake;

#ifdef Q3N_TEST_ALLOC_FAIL
static bool fake_alloc_fail;

void *q3n_test_malloc(size_t length)
{
	if (fake_alloc_fail)
		return NULL;
	return calloc(1, length);
}
#endif

int q3n_hw_read_page(struct q3n *q3n, uint32_t page, void *data, void *oob,
			     bool raw, struct q3n_ecc_result *result)
{
	(void)q3n;
	(void)page;
	(void)data;
	(void)oob;
	(void)raw;
	(void)result;
	return -EOPNOTSUPP;
}

int q3n_hw_program_page(struct q3n *q3n, uint32_t page,
			const void *data, const void *oob)
{
	(void)q3n;
	(void)page;
	(void)data;
	(void)oob;
	return -EOPNOTSUPP;
}

int q3n_hw_read_oob(struct q3n *q3n, uint32_t page, void *oob)
{
	(void)q3n;
	(void)page;
	(void)oob;
	return -EOPNOTSUPP;
}

int q3n_hw_program_oob(struct q3n *q3n, uint32_t page, const void *oob)
{
	(void)q3n;
	(void)page;
	(void)oob;
	return -EOPNOTSUPP;
}

int q3n_hw_erase_block(struct q3n *q3n, uint32_t block)
{
	(void)q3n;
	(void)block;
	return -EOPNOTSUPP;
}

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

static Q3N_TEST_UNUSED void expect_mem(const char *name, const void *got, const void *want,
			       size_t length)
{
	if (memcmp(got, want, length)) {
		fprintf(stderr, "FAIL: %s: buffers differ\n", name);
		exit(1);
	}
}

static Q3N_TEST_UNUSED void fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.read_result.done_mask = 0x0f;
}

static void fake_call(enum fake_op op, const struct q3n_mp_addr *addr,
			  bool raw)
{
	if (fake.calls_count == ARRAY_SIZE(fake.calls)) {
		fprintf(stderr, "FAIL: fake call log overflow\n");
		exit(1);
	}
	fake.calls[fake.calls_count++] = (struct fake_call) {
		.op = op,
		.addr = *addr,
		.raw = raw,
	};
}

static void expect_call(size_t index, enum fake_op op, uint32_t die,
			uint32_t block, uint32_t page, bool raw)
{
	const struct fake_call *call;

	if (index >= fake.calls_count)
		fail("missing fake call", fake.calls_count, index + 1);
	call = &fake.calls[index];
	if (call->op != op || call->addr.die != die ||
	    call->addr.block_in_plane != block ||
	    call->addr.page_in_block != page || call->raw != raw) {
		fprintf(stderr, "FAIL: unexpected fake call %zu\n", index);
		exit(1);
	}
}

int q3n_hw_mp_read_page(struct q3n *q3n, const struct q3n_mp_addr *addr,
			void *data, bool raw, struct q3n_mp_result *result)
{
	(void)q3n;
	fake_call(FAKE_READ_PAGE, addr, raw);
	memset(data, 0x5a, MP_MAIN_SIZE);
	*result = fake.read_result;
	if (fake.main_ret) {
		result->done_mask = 0x0d;
		result->fail_mask = 0x02;
	}
	fake.main_result = *result;
	return fake.main_ret;
}

int q3n_hw_mp_program_page(struct q3n *q3n, const struct q3n_mp_addr *addr,
			   const void *data, struct q3n_mp_result *result)
{
	(void)q3n;
	fake_call(FAKE_PROGRAM_PAGE, addr, false);
	memcpy(fake.last_main, data, sizeof(fake.last_main));
	memset(result, 0, sizeof(*result));
	result->done_mask = fake.main_ret ? 0x0d : 0x0f;
	result->fail_mask = fake.main_ret ? 0x02 : 0;
	fake.main_result = *result;
	return fake.main_ret;
}

int q3n_hw_mp_read_oob(struct q3n *q3n, const struct q3n_mp_addr *addr,
			   void *oob, struct q3n_mp_result *result)
{
	(void)q3n;
	fake_call(FAKE_READ_OOB, addr, false);
	memcpy(oob, fake.read_oob, sizeof(fake.read_oob));
	memset(result, 0, sizeof(*result));
	result->done_mask = fake.oob_ret ? 0x0d : 0x0f;
	result->fail_mask = fake.oob_ret ? 0x02 : 0;
	return fake.oob_ret;
}

int q3n_hw_mp_program_oob(struct q3n *q3n, const struct q3n_mp_addr *addr,
			  const void *oob, struct q3n_mp_result *result)
{
	(void)q3n;
	fake_call(FAKE_PROGRAM_OOB, addr, false);
	memcpy(fake.last_oob, oob, sizeof(fake.last_oob));
	memset(result, 0, sizeof(*result));
	result->done_mask = fake.oob_ret ? 0x0d : 0x0f;
	result->fail_mask = fake.oob_ret ? 0x02 : 0;
	return fake.oob_ret;
}

int q3n_hw_mp_erase_group(struct q3n *q3n, const struct q3n_mp_addr *addr,
			  struct q3n_mp_result *result)
{
	(void)q3n;
	fake_call(FAKE_ERASE, addr, false);
	memset(result, 0, sizeof(*result));
	result->done_mask = fake.erase_ret ? 0x0d : 0x0f;
	result->fail_mask = fake.erase_ret ? 0x02 : 0;
	return fake.erase_ret;
}

static struct q3n make_q3n(void)
{
	static const struct q3n_flash_topology topology = {
		.dies = 2,
		.planes_per_die = 4,
		.blocks_per_plane = 247,
		.data_blocks_per_plane = 208,
		.pages_per_block = 1600,
	};
	struct q3n q3n = {
		.physical_geometry = {
			.writesize = 16384,
			.oobsize = 1024,
			.pages_per_block = 1600,
			.blocks = 1976,
		},
	};

	expect_u64("MP typed page init", q3n_page_layer_init(&q3n,
		Q3N_MODE_MULTIPLANE, 0, &topology), 0);
	expect_u64("MP scratch is 4096 bytes", q3n.multiplane_oob_scratch != NULL,
		   true);
	return q3n;
}

static Q3N_TEST_UNUSED void test_mapping_and_ecc(void)
{
	struct q3n q3n;
	struct q3n_page_result result;
	uint8_t data[MP_MAIN_SIZE];
	uint8_t oob[MP_OOB_SIZE];

	fake_reset();
	q3n = make_q3n();
	fake.read_result.plane[0].ecc.max_bitflips = 3;
	fake.read_result.plane[1].ecc.max_bitflips = 7;
	fake.read_result.plane[2].ecc.max_bitflips = 2;
	fake.read_result.plane[3].ecc.max_bitflips = 5;
	fake.read_result.plane[0].ecc.corrected_bits = 3;
	fake.read_result.plane[1].ecc.corrected_bits = 10;
	fake.read_result.plane[2].ecc.corrected_bits = 1;
	fake.read_result.plane[3].ecc.corrected_bits = 4;
	expect_u64("read first MP page", q3n_page_read(&q3n, 0, data, oob,
		   false, &result), 0);
	expect_u64("main and OOB group reads", fake.calls_count, 2);
	expect_call(0, FAKE_READ_PAGE, 0, 0, 0, false);
	expect_call(1, FAKE_READ_OOB, 0, 0, 0, false);
	expect_u64("MP maximum ECC", result.max_bitflips, 7);
	expect_u64("MP sum corrected ECC", result.corrected_bits, 18);
	expect_u64("MP no failed plane", result.failed_plane_mask, 0);

	fake_reset();
	expect_u64("read die boundary", q3n_page_read(&q3n, 1600, data, NULL,
		   false, &result), 0);
	expect_call(0, FAKE_READ_PAGE, 1, 0, 0, false);
	fake_reset();
	expect_u64("read final page", q3n_page_read(&q3n, 665599, data, NULL,
		   false, &result), 0);
	expect_call(0, FAKE_READ_PAGE, 1, 207, 1599, false);

	q3n_page_layer_cleanup(&q3n);
}

static Q3N_TEST_UNUSED void test_errors_raw_program_and_erase(void)
{
	struct q3n q3n;
	struct q3n_page_result result;
	uint8_t data[MP_MAIN_SIZE];
	uint8_t oob[MP_OOB_SIZE];

	fake_reset();
	q3n = make_q3n();
	fake.read_result.plane[1].ecc.status = Q3N_ECC_STATUS_UNCORRECTABLE;
	fake.read_result.plane[3].ecc.status = Q3N_ECC_STATUS_UNCORRECTABLE;
	expect_u64("uncorrectable MP read transport", q3n_page_read(&q3n, 0,
		   data, NULL, false, &result), 0);
	expect_u64("uncorrectable failed plane mask", result.failed_plane_mask,
		   0x0a);
	expect_u64("uncorrectable failed pages", result.failed_data_pages, 2);

	fake_reset();
	fake.main_ret = -EIO;
	expect_u64("MP media failure wins", q3n_page_read(&q3n, 0, data, NULL,
		   false, &result), (uint64_t)-EIO);
	expect_u64("MP media failure has transport fail mask",
		   fake.main_result.fail_mask, 0x02);

	fake_reset();
	fake.read_result.plane[1].ecc.status = Q3N_ECC_STATUS_UNCORRECTABLE;
	expect_u64("MP raw read", q3n_page_read(&q3n, 0, data, NULL, true,
		   &result), 0);
	expect_call(0, FAKE_READ_PAGE, 0, 0, 0, true);
	expect_u64("raw result max", result.max_bitflips, 0);
	expect_u64("raw result failed", result.failed_plane_mask, 0);

	fake_reset();
	memset(data, 0xff, sizeof(data));
	expect_u64("all-FF MP main still programs", q3n_page_write(&q3n, 0,
		   data, NULL), 0);
	expect_u64("all-FF MP command count", fake.calls_count, 1);
	expect_call(0, FAKE_PROGRAM_PAGE, 0, 0, 0, false);

	fake_reset();
	fake.main_ret = -EIO;
	expect_u64("main failure", q3n_page_write(&q3n, 0, data, oob),
		   (uint64_t)-EIO);
	expect_u64("main failure skips OOB", fake.calls_count, 1);

	fake_reset();
	fake.oob_ret = -EIO;
	expect_u64("OOB failure after main", q3n_page_write(&q3n, 0, data, oob),
		   (uint64_t)-EIO);
	expect_u64("OOB failure no rollback", fake.calls_count, 2);

	fake_reset();
	fake.erase_ret = -EIO;
	expect_u64("partial group erase failure", q3n_page_erase_block(&q3n, 0),
		   (uint64_t)-EIO);
	expect_u64("one group erase only", fake.calls_count, 1);
	expect_call(0, FAKE_ERASE, 0, 0, 0, false);

	q3n_page_layer_cleanup(&q3n);
}

static Q3N_TEST_UNUSED void test_bbm_and_free_regions(void)
{
	struct q3n q3n;
	uint8_t oob[MP_OOB_SIZE];
	uint8_t original[MP_OOB_SIZE];
	uint32_t offset, length;

	fake_reset();
	q3n = make_q3n();
	memset(fake.read_oob, 0xff, sizeof(fake.read_oob));
	fake.read_oob[0] = 0xfe;
	fake.read_oob[1024] = 0xfd;
	fake.read_oob[2048] = 0xf7;
	fake.read_oob[3072] = 0xef;
	expect_u64("first-page OOB read", q3n_page_read_oob(&q3n, 0, oob), 0);
	expect_u64("BBM is folded", oob[0], 0xe4);
	expect_u64("other first BBM remains", oob[1024], 0xfd);
	expect_u64("third BBM remains", oob[2048], 0xf7);
	expect_u64("fourth BBM remains", oob[3072], 0xef);

	memset(oob, 0xff, sizeof(oob));
	oob[0] = 0xfe;
	oob[1024] = 0xfd;
	oob[2048] = 0xf7;
	oob[3072] = 0xef;
	memcpy(original, oob, sizeof(oob));
	fake_reset();
	expect_u64("first-page OOB write", q3n_page_write_oob(&q3n, 0, oob), 0);
	expect_mem("caller OOB untouched", oob, original, sizeof(oob));
	expect_u64("normalized BBM 0", fake.last_oob[0], 0xe4);
	expect_u64("normalized BBM 1", fake.last_oob[1024], 0xe4);
	expect_u64("normalized BBM 2", fake.last_oob[2048], 0xe4);
	expect_u64("normalized BBM 3", fake.last_oob[3072], 0xe4);

	oob[0] = 0x11;
	oob[1024] = 0x22;
	fake_reset();
	expect_u64("non-first OOB write", q3n_page_write_oob(&q3n, 1, oob), 0);
	expect_u64("non-first BBM unchanged", fake.last_oob[0], 0x11);
	expect_u64("non-first second BBM unchanged", fake.last_oob[1024], 0x22);

	expect_u64("free 0", q3n_multiplane_oob_free_region(0, &offset, &length), 0);
	expect_u64("free 0 offset", offset, 1);
	expect_u64("free 0 length", length, 1023);
	expect_u64("free 3", q3n_multiplane_oob_free_region(3, &offset, &length), 0);
	expect_u64("free 3 offset", offset, 3073);
	expect_u64("free 3 length", length, 1023);
	expect_u64("free out of range", q3n_multiplane_oob_free_region(4, &offset, &length),
		   (uint64_t)-ERANGE);

	q3n_page_layer_cleanup(&q3n);
}

#ifdef Q3N_TEST_ALLOC_FAIL
static void test_init_allocation_failure_cleans_both_scratches(void)
{
	const struct q3n_flash_topology topology = {
		.dies = 2,
		.planes_per_die = 4,
		.blocks_per_plane = 247,
		.data_blocks_per_plane = 208,
		.pages_per_block = 1600,
	};
	struct q3n q3n = {
		.physical_geometry = {
			.writesize = 16384,
			.oobsize = 1024,
			.pages_per_block = 1600,
			.blocks = 1976,
		},
	};

	q3n.parity_scratch = calloc(1, 1);
	q3n.multiplane_oob_scratch = calloc(1, 1);
	if (!q3n.parity_scratch || !q3n.multiplane_oob_scratch) {
		fprintf(stderr, "FAIL: allocation-fixture setup\n");
		exit(1);
	}
	fake_alloc_fail = true;
	expect_u64("MP scratch allocation failure",
		q3n_page_layer_init(&q3n, Q3N_MODE_MULTIPLANE, 0, &topology),
		(uint64_t)-ENOMEM);
	expect_u64("failure clears page ops", (uintptr_t)q3n.page_ops, 0);
	expect_u64("failure resets mode", q3n.storage_mode, Q3N_MODE_IDENTITY);
	expect_u64("failure frees parity scratch", (uintptr_t)q3n.parity_scratch, 0);
	expect_u64("failure frees MP scratch", (uintptr_t)q3n.multiplane_oob_scratch,
		   0);
}
#endif

int main(void)
{
#ifdef Q3N_TEST_ALLOC_FAIL
	test_init_allocation_failure_cleans_both_scratches();
#else
	test_mapping_and_ecc();
	test_errors_raw_program_and_erase();
	test_bbm_and_free_regions();
#endif
	puts("ok: Q3N multi-plane logical operations verified");
	return 0;
}
