/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QEMU_3DNAND_INTERNAL_H
#define __QEMU_3DNAND_INTERNAL_H

#include <linux/debugfs.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/pci.h>
#include <linux/workqueue.h>

#include "qemu_3dnand.h"
#include "qemu_3dnand_priv.h"

#define Q3N_DATA_PAGES		7
#define Q3N_STRIPE_PAGES	(Q3N_DATA_PAGES + 1)
#define Q3N_RAID_LANES		Q3N_DATA_PAGES
#define Q3N_NAND_ID_LEN		8

struct q3n_device_desc;

struct qemu_3dnand_data_block_meta {
	u32 generation;
	bool bad;
	struct q3n_block_barrier parity_barrier;
};

struct qemu_3dnand_parity_entry {
	u32 physical_block;
	u32 page;
	u32 data_block_generation[Q3N_RAID_LANES];
	u32 parity_version;
	u64 sequence;
	bool valid;
};

struct qemu_3dnand {
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_size;
	struct mutex mtd_lock;
	u32 cap;
	u8 nand_id[Q3N_NAND_ID_LEN];
	u8 nand_id_len;
	const struct q3n_device_desc *device;
	enum q3n_raid_level raid_level;

	struct nand_controller controller;
	struct nand_chip chip;
	struct mtd_info *mtd;
	struct nand_flash_dev scan_ids[2];
	bool scanned;
	bool core_markbad;
	struct {
		u8 data[Q3N_NAND_ID_LEN];
		u8 len;
		u8 pos;
		u32 erase_page;
		int error;
		bool erase_pending;
	} legacy;

	struct q3n_mp_io mp;
	struct q3n_geometry profile_geometry;
	u8 *mp_buf[Q3N_PLANES_PER_DIE];
	u8 *mp_oob[Q3N_PLANES_PER_DIE];
	enum q3n_stripe_state *profile_state;
	u32 profile_leb_count;

	struct q3n_sched sched;
	struct workqueue_struct *parity_wq;
	wait_queue_head_t parity_cancel_waitq;
	wait_queue_head_t parity_pause_waitq;
	atomic_t parity_paused;
	atomic_t parity_continuation_paused;
	atomic_t fail_next_parity_queue;
	u32 parity_pause_block;
	u32 parity_continuation_pause_block;
	u32 parity_continuation_pause_class;
	bool parity_pause_enable;
	bool parity_continuation_pause_enable;
	u8 *data_page_valid;
	struct qemu_3dnand_data_block_meta *data_meta;
	struct qemu_3dnand_parity_entry *parity_index;

	struct dentry *debugfs_dir;
	u8 *page_buf;
	u8 *raid_buf;
	u32 page_size;
	u32 oob_size;
	u32 ecc_step_size;
	u32 ecc_strength;
	u32 ldpc_bytes_per_step;
	u32 ldpc_steps;
	u32 pages_per_block;
	u32 blocks_per_plane;
	u32 data_blocks_per_plane;
	u32 parity_blocks_per_plane;
	u32 metadata_blocks_per_plane;
	u32 reserve_blocks_per_plane;
	u32 data_block_count;
	u32 parity_block_count;
	u32 raid_group_count;
	u64 parity_next_page;
	u64 parity_sequence;

	u64 parity_written;
	u64 parity_stale;
	u64 raid_recovered;
	u64 raid_failed;
	u64 raid_source_corrected_bits;
	u64 metadata_degraded;
	u8 last_parity_plane;
	u64 background_ecc_corrected_bits;
	u64 generation_updates;
	atomic64_t protected_stripes;
	atomic64_t unprotected_stripes;
	atomic64_t failed_stripes;
};

struct qemu_3dnand_parity_work {
	struct work_struct work;
	struct qemu_3dnand *q3n;
	struct q3n_request request;
	struct q3n_parity_rebuild rebuild;
	u32 block;
	u32 stripe;
	u8 *page_buf;
	bool failure_accounted;
	bool request_queued;
};

u32 q3n_hw_readl(struct qemu_3dnand *q3n, u32 reg);
void q3n_hw_writel(struct qemu_3dnand *q3n, u32 reg, u32 value);
/* Every hardware entry point suffixed with _locked requires mtd_lock held. */
int q3n_hw_wait_ready_locked(struct qemu_3dnand *q3n);
loff_t q3n_hw_phys_addr(struct qemu_3dnand *q3n, u32 block, u32 page);
int q3n_hw_read_id_locked(struct qemu_3dnand *q3n, u8 *id, size_t len);
int q3n_hw_read_page_locked(struct qemu_3dnand *q3n, u32 block, u32 page,
			    u8 *buf, u32 op_class, struct q3n_ecc_result *ecc);
int q3n_hw_read_oob_locked(struct qemu_3dnand *q3n, u32 block, u32 page,
			   u8 oob[Q3N_LOGICAL_OOB_SIZE], u32 op_class);
int q3n_hw_program_page_locked(struct qemu_3dnand *q3n, u32 block, u32 page,
			       const u8 *buf, u32 op_class);
int q3n_hw_program_oob_locked(struct qemu_3dnand *q3n, u32 block, u32 page,
			      const u8 oob[Q3N_LOGICAL_OOB_SIZE], u32 op_class);
int q3n_hw_erase_block_locked(struct qemu_3dnand *q3n, u32 block);
int q3n_hw_get_block_status_locked(struct qemu_3dnand *q3n, u32 block,
				   u32 *status);
void q3n_hw_build_bad_block_oob(u8 *logical_oob);
void q3n_hw_account_background_ecc(struct qemu_3dnand *q3n,
				   const struct q3n_ecc_result *ecc);
void q3n_hw_account_foreground_ecc(struct qemu_3dnand *q3n,
				   struct mtd_req_stats *stats,
				   const struct q3n_ecc_result *ecc);

int q3n_profile_group(struct qemu_3dnand *q3n, loff_t addr,
		      struct q3n_raid_group *group, u64 *leb_out,
		      u8 *data_slot, u32 *column);
void q3n_profile_buffers(struct qemu_3dnand *q3n,
			 const struct q3n_raid_group *group,
			 struct q3n_mp_buffers *buffers, bool oob);
void q3n_profile_invalidate_leb(struct qemu_3dnand *q3n, u64 leb);
int q3n_profile_read_page_locked(struct qemu_3dnand *q3n, loff_t from,
				 u8 *buf);
int q3n_profile_write_page_locked(struct qemu_3dnand *q3n, loff_t to,
				  const u8 *buf);
int q3n_profile_erase_page_locked(struct mtd_info *mtd,
				  struct erase_info *instr);
void q3n_profile_sync(struct mtd_info *mtd);
int q3n_profile_block_bad(struct mtd_info *mtd, loff_t ofs);
int q3n_profile_block_markbad(struct mtd_info *mtd, loff_t ofs);

void qemu_3dnand_decode_logical(struct qemu_3dnand *q3n, loff_t addr,
				u32 *block, u32 *page, u32 *column);
int q3n_serial_read_page_locked(struct qemu_3dnand *q3n, u32 block, u32 page,
				u8 *buf, struct mtd_req_stats *stats,
				struct q3n_ecc_result *ecc);
int q3n_serial_write_page(struct qemu_3dnand *q3n, u32 block, u32 page,
			  const u8 *data, const u8 *oob, u32 ooboffs,
			  size_t ooblen, bool *data_programmed,
			  bool *oob_programmed);
void q3n_serial_invalidate_block_parity(struct qemu_3dnand *q3n, u32 block);
int q3n_serial_cancel_block_parity(struct qemu_3dnand *q3n, u32 block);
int q3n_serial_mtd_read(struct mtd_info *mtd, loff_t from, size_t len,
			size_t *retlen, u_char *buf,
			struct mtd_req_stats *stats);
int q3n_serial_mtd_read_oob(struct mtd_info *mtd, loff_t from,
			    struct mtd_oob_ops *ops);
int q3n_serial_mtd_write(struct mtd_info *mtd, loff_t to, size_t len,
			 size_t *retlen, const u_char *buf);
int q3n_serial_mtd_write_oob(struct mtd_info *mtd, loff_t to,
			     struct mtd_oob_ops *ops);
int q3n_serial_mtd_erase(struct mtd_info *mtd, struct erase_info *instr);
void q3n_serial_sync(struct mtd_info *mtd);
int q3n_serial_block_bad(struct mtd_info *mtd, loff_t ofs);
int q3n_serial_block_markbad(struct mtd_info *mtd, loff_t ofs);

int q3n_nand_register(struct qemu_3dnand *q3n);
void q3n_nand_unregister(struct qemu_3dnand *q3n);
void q3n_nand_cleanup(struct qemu_3dnand *q3n);
void q3n_debugfs_init(struct qemu_3dnand *q3n);
void q3n_debugfs_remove(struct qemu_3dnand *q3n);
void q3n_debugfs_unpause(struct qemu_3dnand *q3n);

const struct q3n_device_desc *q3n_device_match(const u8 *id, size_t len);
int q3n_device_validate(struct qemu_3dnand *q3n,
			const struct q3n_device_desc *device);
int q3n_device_build_scan_id(struct qemu_3dnand *q3n,
			     struct nand_flash_dev *scan_id, u32 writesize,
			     u32 oobsize, u32 erasesize, u64 size);
const char *q3n_device_name(const struct q3n_device_desc *device);
u8 q3n_device_dies(const struct q3n_device_desc *device);
u8 q3n_device_planes_per_die(const struct q3n_device_desc *device);

#endif /* __QEMU_3DNAND_INTERNAL_H */
