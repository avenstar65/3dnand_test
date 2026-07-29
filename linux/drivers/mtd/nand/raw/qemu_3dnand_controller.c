// SPDX-License-Identifier: GPL-2.0
#ifdef Q3N_HOST_TEST
#include <errno.h>
#include <string.h>
#else
#include <linux/errno.h>
#include <linux/mtd/rawnand.h>
#include <linux/string.h>
#endif

#include "qemu_3dnand_controller.h"
#include "qemu_3dnand_ecc.h"
#include "qemu_3dnand_hw.h"
#include "qemu_3dnand_priv.h"

#ifdef Q3N_HOST_TEST
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
#endif

static void q3n_legacy_clear_data(struct q3n_legacy_state *legacy)
{
	legacy->data_len = 0;
	legacy->data_pos = 0;
}

static void q3n_legacy_record_error(struct q3n *q3n, int error)
{
	if (!q3n || error >= 0 || q3n->legacy.error)
		return;

	q3n_legacy_clear_data(&q3n->legacy);
	q3n->legacy.error = error;
}

void q3n_legacy_state_init(struct q3n *q3n)
{
	if (!q3n)
		return;

	memset(&q3n->legacy, 0, sizeof(q3n->legacy));
}

static void q3n_legacy_reset(struct q3n *q3n)
{
	int ret;

	ret = q3n_hw_reset(q3n);
	if (ret) {
		q3n_legacy_record_error(q3n, ret);
		return;
	}

	q3n_legacy_state_init(q3n);
	q3n->retry_mode = 0;
}

static void q3n_legacy_read_id(struct q3n *q3n, int column)
{
	int ret;

	q3n_legacy_clear_data(&q3n->legacy);
	if (column) {
		q3n_legacy_record_error(q3n, -EINVAL);
		return;
	}

	ret = q3n_hw_read_id(q3n, q3n->legacy.data,
			     sizeof(q3n->legacy.data));
	if (ret) {
		q3n_legacy_record_error(q3n, ret);
		return;
	}

	q3n->legacy.data_len = sizeof(q3n->legacy.data);
}

static void q3n_legacy_status(struct q3n *q3n)
{
	u8 status;
	int ret;

	q3n_legacy_clear_data(&q3n->legacy);
	ret = q3n_hw_read_status(q3n, &status);
	if (ret) {
		q3n_legacy_record_error(q3n, ret);
		return;
	}

	q3n->legacy.data[0] = status;
	q3n->legacy.data_len = 1;
}

static void q3n_legacy_erase1(struct q3n *q3n, int page_addr)
{
	u64 pages;

	q3n->legacy.erase_pending = false;
	if (page_addr < 0 || !q3n->geometry.pages_per_block ||
	    !q3n->geometry.blocks) {
		q3n_legacy_record_error(q3n, -EINVAL);
		return;
	}

	pages = (u64)q3n->geometry.pages_per_block * q3n->geometry.blocks;
	if ((u64)page_addr >= pages ||
	    (u32)page_addr % q3n->geometry.pages_per_block) {
		q3n_legacy_record_error(q3n, -EINVAL);
		return;
	}

	q3n->legacy.erase_page = page_addr;
	q3n->legacy.erase_pending = true;
}

static void q3n_legacy_erase2(struct q3n *q3n)
{
	u32 block;
	int ret;

	if (!q3n->legacy.erase_pending) {
		q3n_legacy_record_error(q3n, -EINVAL);
		return;
	}

	block = q3n->legacy.erase_page / q3n->geometry.pages_per_block;
	q3n->legacy.erase_pending = false;
	ret = q3n_hw_erase_block(q3n, block);
	if (ret)
		q3n_legacy_record_error(q3n, ret);
}

void q3n_legacy_command(struct q3n *q3n, unsigned int command,
			int column, int page_addr)
{
	if (!q3n)
		return;
	if (q3n->legacy.error)
		return;

	switch (command) {
	case NAND_CMD_RESET:
		q3n_legacy_reset(q3n);
		break;
	case NAND_CMD_READID:
		q3n_legacy_read_id(q3n, column);
		break;
	case NAND_CMD_STATUS:
		q3n_legacy_status(q3n);
		break;
	case NAND_CMD_ERASE1:
		q3n_legacy_erase1(q3n, page_addr);
		break;
	case NAND_CMD_ERASE2:
		q3n_legacy_erase2(q3n);
		break;
	case NAND_CMD_READ0:
	case NAND_CMD_READOOB:
	case NAND_CMD_SEQIN:
	case NAND_CMD_PAGEPROG:
		break;
	case NAND_CMD_RNDOUT:
	case NAND_CMD_RNDIN:
	default:
		q3n_legacy_record_error(q3n, -EOPNOTSUPP);
		break;
	}
}

u8 q3n_legacy_read_byte_value(struct q3n *q3n)
{
	struct q3n_legacy_state *legacy;

	if (!q3n || q3n->legacy.error)
		return 0xff;

	legacy = &q3n->legacy;
	if (legacy->data_pos >= legacy->data_len)
		return 0xff;

	return legacy->data[legacy->data_pos++];
}

void q3n_legacy_read_buffer(struct q3n *q3n, u8 *buf, int len)
{
	int i;

	if (!q3n || !buf || len < 0) {
		q3n_legacy_record_error(q3n, -EINVAL);
		return;
	}

	for (i = 0; i < len; i++)
		buf[i] = q3n_legacy_read_byte_value(q3n);
}

void q3n_legacy_write_buffer(struct q3n *q3n, const u8 *buf, int len)
{
	if (!q3n)
		return;
	if (!buf || len < 0)
		q3n_legacy_record_error(q3n, -EINVAL);
	else
		q3n_legacy_record_error(q3n, -EOPNOTSUPP);
}

int q3n_legacy_wait(struct q3n *q3n)
{
	u8 status;
	int ret;

	if (!q3n)
		return -EINVAL;

	if (q3n->legacy.error) {
		ret = q3n->legacy.error;
		q3n->legacy.error = 0;
		return ret;
	}

	ret = q3n_hw_read_status(q3n, &status);
	if (ret)
		return ret;
	return status;
}

int q3n_legacy_select(struct q3n *q3n, int chipnr)
{
	if (!q3n)
		return -EINVAL;
	if (chipnr == 0 || chipnr == -1)
		return 0;

	q3n_legacy_record_error(q3n, -EINVAL);
	return -EINVAL;
}

#ifndef Q3N_HOST_TEST
static void q3n_cmdfunc(struct nand_chip *chip, unsigned int command,
			int column, int page_addr)
{
	struct q3n *q3n = nand_to_q3n(chip);

	mutex_lock(&q3n->lock);
	q3n_legacy_command(q3n, command, column, page_addr);
	mutex_unlock(&q3n->lock);
}

static int q3n_waitfunc(struct nand_chip *chip)
{
	struct q3n *q3n = nand_to_q3n(chip);
	int ret;

	mutex_lock(&q3n->lock);
	ret = q3n_legacy_wait(q3n);
	mutex_unlock(&q3n->lock);
	return ret;
}

static u8 q3n_read_byte(struct nand_chip *chip)
{
	struct q3n *q3n = nand_to_q3n(chip);
	u8 value;

	mutex_lock(&q3n->lock);
	value = q3n_legacy_read_byte_value(q3n);
	mutex_unlock(&q3n->lock);
	return value;
}

static void q3n_read_buf(struct nand_chip *chip, u8 *buf, int len)
{
	struct q3n *q3n = nand_to_q3n(chip);

	mutex_lock(&q3n->lock);
	q3n_legacy_read_buffer(q3n, buf, len);
	mutex_unlock(&q3n->lock);
}

static void q3n_write_buf(struct nand_chip *chip, const u8 *buf, int len)
{
	struct q3n *q3n = nand_to_q3n(chip);

	mutex_lock(&q3n->lock);
	q3n_legacy_write_buffer(q3n, buf, len);
	mutex_unlock(&q3n->lock);
}

static void q3n_select_chip(struct nand_chip *chip, int chipnr)
{
	struct q3n *q3n = nand_to_q3n(chip);

	mutex_lock(&q3n->lock);
	q3n_legacy_select(q3n, chipnr);
	mutex_unlock(&q3n->lock);
}

void q3n_controller_legacy_init(struct nand_chip *chip)
{
	struct q3n *q3n = nand_to_q3n(chip);

	q3n_legacy_state_init(q3n);
	chip->legacy.cmdfunc = q3n_cmdfunc;
	chip->legacy.waitfunc = q3n_waitfunc;
	chip->legacy.read_byte = q3n_read_byte;
	chip->legacy.read_buf = q3n_read_buf;
	chip->legacy.write_buf = q3n_write_buf;
	chip->legacy.select_chip = q3n_select_chip;
}

static int q3n_attach_chip(struct nand_chip *chip)
{
	return q3n_ecc_init(nand_to_q3n(chip));
}

const struct nand_controller_ops q3n_controller_ops = {
	.attach_chip = q3n_attach_chip,
};
#endif
