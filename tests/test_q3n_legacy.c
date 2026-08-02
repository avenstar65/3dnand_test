#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu_3dnand_controller.h"
#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_priv.h"

#define NAND_CMD_READ0		0x00
#define NAND_CMD_RNDOUT		0x05
#define NAND_CMD_PAGEPROG	0x10
#define NAND_CMD_READOOB	0x50
#define NAND_CMD_ERASE1		0x60
#define NAND_CMD_STATUS		0x70
#define NAND_CMD_SEQIN		0x80
#define NAND_CMD_RNDIN		0x85
#define NAND_CMD_READID		0x90
#define NAND_CMD_ERASE2		0xd0
#define NAND_CMD_RESET		0xff

static const u8 expected_id[8] = {
	0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44,
};
static int reset_result;
static int read_id_result;
static int status_result;
static int erase_result;
static unsigned int reset_calls;
static unsigned int read_id_calls;
static unsigned int status_calls;
static unsigned int erase_calls;
static unsigned int page_erase_calls;
static size_t last_id_len;
static u32 last_erase_block;
static u32 last_logical_block;
static u8 fake_status;

static void expect_u64(const char *name, uint64_t got, uint64_t want)
{
	if (got != want) {
		fprintf(stderr, "FAIL: %s: got %llu, want %llu\n", name,
			(unsigned long long)got, (unsigned long long)want);
		exit(1);
	}
}

static void reset_fake_hw(void)
{
	reset_result = 0;
	read_id_result = 0;
	status_result = 0;
	erase_result = 0;
	reset_calls = 0;
	read_id_calls = 0;
	status_calls = 0;
	erase_calls = 0;
	page_erase_calls = 0;
	last_id_len = 0;
	last_erase_block = UINT32_MAX;
	last_logical_block = UINT32_MAX;
	fake_status = 0xc0;
}

static void init_q3n(struct q3n *q3n)
{
	memset(q3n, 0, sizeof(*q3n));
	q3n->geometry.writesize = 16384;
	q3n->geometry.oobsize = 1024;
	q3n->geometry.pages_per_block = 1600;
	q3n->geometry.blocks = 1664;
	q3n_legacy_state_init(q3n);
}

int q3n_hw_reset(struct q3n *q3n)
{
	(void)q3n;
	reset_calls++;
	return reset_result;
}

int q3n_hw_read_id(struct q3n *q3n, u8 *id, size_t len)
{
	(void)q3n;
	read_id_calls++;
	last_id_len = len;
	if (read_id_result)
		return read_id_result;
	memcpy(id, expected_id, len);
	return 0;
}

int q3n_hw_read_status(struct q3n *q3n, u8 *status)
{
	(void)q3n;
	status_calls++;
	if (status_result)
		return status_result;
	*status = fake_status;
	return 0;
}

int q3n_hw_erase_block(struct q3n *q3n, u32 block)
{
	(void)q3n;
	erase_calls++;
	last_erase_block = block;
	return erase_result;
}

int q3n_page_erase_block(struct q3n *q3n, u32 logical_block)
{
	page_erase_calls++;
	last_logical_block = logical_block;
	return q3n_hw_erase_block(q3n, logical_block);
}

static void test_reset_clears_transaction_and_retry_state(void)
{
	struct q3n q3n;

	reset_fake_hw();
	init_q3n(&q3n);
	q3n.retry_mode = 3;
	q3n.legacy.data[0] = 0x12;
	q3n.legacy.data_len = 1;
	q3n.legacy.erase_pending = true;
	q3n.legacy.erase_page = 1600;

	q3n_legacy_command(&q3n, NAND_CMD_RESET, -1, -1);

	expect_u64("reset calls", reset_calls, 1);
	expect_u64("retry mode after reset", q3n.retry_mode, 0);
	expect_u64("staging length after reset", q3n.legacy.data_len, 0);
	expect_u64("erase pending after reset", q3n.legacy.erase_pending, 0);
	expect_u64("wait after reset", q3n_legacy_wait(&q3n), fake_status);
}

static void test_read_id_reloads_full_staging_buffer(void)
{
	struct q3n q3n;
	u8 id[8] = { 0 };

	reset_fake_hw();
	init_q3n(&q3n);
	q3n_legacy_command(&q3n, NAND_CMD_READID, 0, -1);
	expect_u64("READID call count", read_id_calls, 1);
	expect_u64("READID hardware length", last_id_len, 8);
	expect_u64("READID first byte", q3n_legacy_read_byte_value(&q3n),
		   expected_id[0]);
	expect_u64("READID second byte", q3n_legacy_read_byte_value(&q3n),
		   expected_id[1]);

	q3n_legacy_command(&q3n, NAND_CMD_READID, 0, -1);
	q3n_legacy_read_buffer(&q3n, id, sizeof(id));
	expect_u64("READID reload call count", read_id_calls, 2);
	if (memcmp(id, expected_id, sizeof(id))) {
		fprintf(stderr, "FAIL: reloaded READID bytes differ\n");
		exit(1);
	}
	expect_u64("READID exhausted byte", q3n_legacy_read_byte_value(&q3n),
		   0xff);
}

static void test_status_uses_one_byte_staging(void)
{
	struct q3n q3n;

	reset_fake_hw();
	init_q3n(&q3n);
	fake_status = 0xc1;

	q3n_legacy_command(&q3n, NAND_CMD_STATUS, -1, -1);

	expect_u64("STATUS hardware calls", status_calls, 1);
	expect_u64("STATUS byte", q3n_legacy_read_byte_value(&q3n), 0xc1);
	expect_u64("STATUS exhausted byte", q3n_legacy_read_byte_value(&q3n),
		   0xff);
}

static void test_erase_is_two_phase_and_uses_logical_page_layer(void)
{
	struct q3n q3n;

	reset_fake_hw();
	init_q3n(&q3n);
	q3n.geometry.pages_per_block = 320;

	q3n_legacy_command(&q3n, NAND_CMD_ERASE1, -1, 319);
	expect_u64("misaligned erase error", q3n_legacy_wait(&q3n),
		   (uint64_t)-EINVAL);
	expect_u64("misaligned erase calls", erase_calls, 0);
	expect_u64("misaligned page erase calls", page_erase_calls, 0);

	q3n_legacy_command(&q3n, NAND_CMD_ERASE1, -1, 320);
	expect_u64("ERASE1 does not erase", erase_calls, 0);
	q3n_legacy_command(&q3n, NAND_CMD_ERASE2, -1, -1);
	expect_u64("ERASE2 calls page layer", page_erase_calls, 1);
	expect_u64("ERASE2 logical block", last_logical_block, 1);
	expect_u64("ERASE2 physical block", last_erase_block, 1);

	q3n_legacy_command(&q3n, NAND_CMD_ERASE1, -1, 532160);
	q3n_legacy_command(&q3n, NAND_CMD_ERASE2, -1, -1);
	expect_u64("last block erase calls", erase_calls, 2);
	expect_u64("last block number", last_erase_block, 1663);

	q3n_legacy_command(&q3n, NAND_CMD_ERASE1, -1, 532480);
	expect_u64("out-of-range erase error", q3n_legacy_wait(&q3n),
		   (uint64_t)-EINVAL);
	expect_u64("out-of-range erase calls", erase_calls, 2);

	q3n_legacy_command(&q3n, NAND_CMD_ERASE2, -1, -1);
	expect_u64("ERASE2 without ERASE1", q3n_legacy_wait(&q3n),
		   (uint64_t)-EINVAL);
}

static void test_first_error_blocks_hardware_until_wait(void)
{
	struct q3n q3n;

	reset_fake_hw();
	init_q3n(&q3n);
	read_id_result = -EIO;

	q3n_legacy_command(&q3n, NAND_CMD_READID, 0, -1);
	q3n_legacy_command(&q3n, NAND_CMD_STATUS, -1, -1);
	q3n_legacy_command(&q3n, 0xab, -1, -1);

	expect_u64("failed READID calls", read_id_calls, 1);
	expect_u64("pending error blocks STATUS", status_calls, 0);
	expect_u64("failed command reads ff",
		   q3n_legacy_read_byte_value(&q3n), 0xff);
	expect_u64("first error returned", q3n_legacy_wait(&q3n),
		   (uint64_t)-EIO);

	fake_status = 0xc0;
	expect_u64("error consumed once", q3n_legacy_wait(&q3n), 0xc0);
	expect_u64("wait reads status after consume", status_calls, 1);
}

static void test_reset_failure_preserves_error(void)
{
	struct q3n q3n;

	reset_fake_hw();
	init_q3n(&q3n);
	q3n.retry_mode = 2;
	reset_result = -ETIMEDOUT;

	q3n_legacy_command(&q3n, NAND_CMD_RESET, -1, -1);

	expect_u64("failed reset call", reset_calls, 1);
	expect_u64("failed reset retains retry mode", q3n.retry_mode, 2);
	expect_u64("failed reset read byte", q3n_legacy_read_byte_value(&q3n),
		   0xff);
	expect_u64("failed reset wait error", q3n_legacy_wait(&q3n),
		   (uint64_t)-ETIMEDOUT);
}

static void test_page_oob_commands_are_sequencing_only(void)
{
	static const unsigned int commands[] = {
		NAND_CMD_READ0,
		NAND_CMD_READOOB,
		NAND_CMD_SEQIN,
		NAND_CMD_PAGEPROG,
	};
	struct q3n q3n;
	size_t i;

	reset_fake_hw();
	init_q3n(&q3n);
	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		q3n_legacy_command(&q3n, commands[i], 32, 1600);

	expect_u64("sequencing reset calls", reset_calls, 0);
	expect_u64("sequencing READID calls", read_id_calls, 0);
	expect_u64("sequencing STATUS calls", status_calls, 0);
	expect_u64("sequencing erase calls", erase_calls, 0);
	expect_u64("sequencing pending error", q3n.legacy.error, 0);
}

static void test_unsupported_io_and_chip_select_report_errors(void)
{
	struct q3n q3n;
	u8 byte = 0x12;

	reset_fake_hw();
	init_q3n(&q3n);
	q3n_legacy_command(&q3n, NAND_CMD_RNDOUT, 0, 0);
	expect_u64("RNDOUT unsupported", q3n_legacy_wait(&q3n),
		   (uint64_t)-EOPNOTSUPP);

	q3n_legacy_command(&q3n, NAND_CMD_RNDIN, 0, 0);
	expect_u64("RNDIN unsupported", q3n_legacy_wait(&q3n),
		   (uint64_t)-EOPNOTSUPP);

	q3n_legacy_write_buffer(&q3n, &byte, 1);
	expect_u64("write buffer unsupported", q3n_legacy_wait(&q3n),
		   (uint64_t)-EOPNOTSUPP);

	expect_u64("select chip zero", q3n_legacy_select(&q3n, 0), 0);
	expect_u64("deselect chip", q3n_legacy_select(&q3n, -1), 0);
	expect_u64("select chip one", q3n_legacy_select(&q3n, 1),
		   (uint64_t)-EINVAL);
	expect_u64("select chip error waits", q3n_legacy_wait(&q3n),
		   (uint64_t)-EINVAL);
}

int main(void)
{
	test_reset_clears_transaction_and_retry_state();
	test_read_id_reloads_full_staging_buffer();
	test_status_uses_one_byte_staging();
	test_erase_is_two_phase_and_uses_logical_page_layer();
	test_first_error_blocks_hardware_until_wait();
	test_reset_failure_preserves_error();
	test_page_oob_commands_are_sequencing_only();
	test_unsupported_io_and_chip_select_report_errors();
	printf("ok: Q3N legacy command state machine verified\n");
	return 0;
}
