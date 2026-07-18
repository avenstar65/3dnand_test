#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_file() {
  [ -f "$repo_root/$1" ] || fail "missing file: $1"
}

assert_executable() {
  [ -x "$repo_root/$1" ] || fail "not executable: $1"
}

assert_contains() {
  file=$1
  pattern=$2
  grep -Eq -- "$pattern" "$repo_root/$file" || fail "$file does not contain pattern: $pattern"
}

assert_not_contains() {
  file=$1
  pattern=$2
  ! grep -Eq -- "$pattern" "$repo_root/$file" || fail "$file unexpectedly contains pattern: $pattern"
}

for file in \
  Dockerfile \
  .dockerignore \
  README.md \
  scripts/lib/common.sh \
  scripts/build-image.sh \
  scripts/shell.sh \
  scripts/fetch-linux.sh \
  scripts/fetch-qemu.sh \
  scripts/configure-kernel.sh \
  scripts/build-kernel.sh \
  scripts/apply-linux-overlay.sh \
  scripts/apply-qemu-overlay.sh \
  scripts/build-qemu.sh \
  scripts/build-rootfs.sh \
  scripts/build-module.sh \
  scripts/run-qemu.sh \
  scripts/gdb-kernel.sh \
  scripts/smoke-test.sh \
  configs/linux/qemu-x86_64-debug.fragment \
  configs/linux/qemu-x86_64-lean.fragment \
  configs/linux/mtd.fragment \
  configs/qemu/x86_64.env \
  rootfs/init \
  rootfs/profile.d/mtd.sh \
  drivers/mtd_demo/Makefile \
  drivers/mtd_demo/mtd_demo.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_map.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c \
  linux/drivers/mtd/nand/raw/Kconfig.qemu_3dnand \
  linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
  qemu/README.md \
  qemu/include/hw/mtd/q3n-media.h \
  qemu/include/hw/mtd/q3n-nand.h \
  qemu/hw/mtd/q3n-media.c \
  qemu/hw/mtd/q3n-nand.c \
  qemu/hw/mtd/q3n-pci.c \
  qemu/hw/mtd/meson.build \
  qemu/hw/mtd/Kconfig; do
  assert_file "$file"
done

for file in \
  scripts/build-image.sh \
  scripts/shell.sh \
  scripts/fetch-linux.sh \
  scripts/fetch-qemu.sh \
  scripts/configure-kernel.sh \
  scripts/build-kernel.sh \
  scripts/apply-linux-overlay.sh \
  scripts/apply-qemu-overlay.sh \
  scripts/build-qemu.sh \
  scripts/build-rootfs.sh \
  scripts/build-module.sh \
  scripts/run-qemu.sh \
  scripts/gdb-kernel.sh \
  scripts/smoke-test.sh \
  rootfs/init \
  rootfs/profile.d/mtd.sh; do
  assert_executable "$file"
done

assert_contains Dockerfile 'qemu-system-x86'
assert_contains Dockerfile 'mtd-utils'
assert_contains Dockerfile 'ARG BASE_IMAGE'
assert_contains Dockerfile 'gcc-x86-64-linux-gnu'
assert_contains Dockerfile 'meson'
assert_contains Dockerfile 'ninja-build'
assert_contains Dockerfile 'libglib2.0-dev'
assert_contains Dockerfile 'libpixman-1-dev'
assert_contains Dockerfile 'busybox-static:amd64'
assert_contains Dockerfile 'mtd-utils:amd64'
assert_contains .dockerignore '^work/'
assert_contains scripts/build-image.sh 'BASE_IMAGE'
assert_contains scripts/configure-kernel.sh 'CROSS_COMPILE'
assert_contains scripts/configure-kernel.sh 'merge_config.sh'
assert_contains scripts/configure-kernel.sh 'apply-linux-overlay.sh'
assert_contains scripts/configure-kernel.sh '"\$merge" -m'
assert_contains scripts/build-kernel.sh 'CROSS_COMPILE'
assert_contains scripts/fetch-linux.sh 'kernel.org'
assert_contains scripts/fetch-qemu.sh 'download.qemu.org'
assert_contains scripts/fetch-linux.sh 'KERNEL_BASE_URL'
assert_contains scripts/build-qemu.sh 'apply-qemu-overlay.sh'
assert_contains scripts/build-qemu.sh 'x86_64-softmmu'
assert_contains scripts/run-qemu.sh 'q3n-nand-pci'
assert_contains scripts/run-qemu.sh '--fresh-nand'
assert_contains scripts/run-qemu.sh '--nand-image'
assert_contains scripts/run-qemu.sh 'q3n-nand-pci,drive=q3n-media'
assert_contains scripts/fetch-linux.sh 'delay-directory-restore'
assert_contains scripts/fetch-linux.sh 'redownload'
assert_contains scripts/fetch-linux.sh 'continue-at'
assert_contains scripts/fetch-linux.sh 'rsync'
assert_contains scripts/fetch-linux.sh 'extract_tmp'
assert_contains scripts/build-rootfs.sh 'ROOTFS_BUSYBOX'
assert_contains scripts/build-rootfs.sh 'x86-64'
assert_contains scripts/build-rootfs.sh '/opt/rootfs-amd64/usr/sbin/'
assert_contains scripts/build-rootfs.sh 'ubiattach'
assert_contains scripts/build-rootfs.sh 'flash_erase'
assert_contains scripts/build-rootfs.sh 'ld-linux-x86-64.so.2'
assert_contains rootfs/init 'poweroff -f'
assert_contains rootfs/init 'debugfs'
assert_contains rootfs/init 'MTD_SMOKE.*ubifs'
assert_contains scripts/run-qemu.sh '-s -S'
assert_contains configs/linux/mtd.fragment 'CONFIG_MTD_NAND_NANDSIM'
assert_contains configs/linux/qemu-x86_64-lean.fragment 'CONFIG_DRM is not set'
assert_contains scripts/configure-kernel.sh 'qemu-x86_64-lean.fragment'
assert_contains drivers/mtd_demo/mtd_demo.c 'get_mtd_device'
assert_contains drivers/mtd_demo/mtd_demo.c 'put_mtd_device'
assert_contains rootfs/profile.d/mtd.sh 'ubifs'
assert_contains rootfs/profile.d/mtd.sh 'ubiattach'
assert_contains rootfs/profile.d/mtd.sh 'ubimkvol'
assert_contains rootfs/profile.d/mtd.sh 'mount -t ubifs'
assert_contains rootfs/profile.d/mtd.sh 'zstd_compress'
assert_contains rootfs/profile.d/mtd.sh 'ubiformat -q'
assert_contains rootfs/profile.d/mtd.sh 'qemu_3dnand'
assert_contains rootfs/profile.d/mtd.sh 'qemu-3dnand'
assert_contains rootfs/profile.d/mtd.sh 'q3n-inject-loss'
assert_contains rootfs/profile.d/mtd.sh 'q3n-stats'
assert_contains rootfs/profile.d/mtd.sh 'q3n-serial-smoke'
assert_contains rootfs/profile.d/mtd.sh 'q3n-generation-smoke'
assert_contains rootfs/profile.d/mtd.sh 'q3n-cancel-barrier-smoke'
assert_contains rootfs/profile.d/mtd.sh 'cancel_writer_pid=\$!'
assert_contains rootfs/profile.d/mtd.sh 'wait "\$cancel_writer_pid"'
assert_contains rootfs/profile.d/mtd.sh 'dd if=/tmp/q3n-cancel\.bin of="\$mtd_dev" bs=16384 count=7 2>/tmp/q3n-cancel-dd\.err[[:space:]]*&[[:space:]]*$'
assert_not_contains rootfs/profile.d/mtd.sh 'kill "\$cancel_writer_pid"'
assert_contains rootfs/profile.d/mtd.sh 'parity_written_before'
assert_contains rootfs/profile.d/mtd.sh 'raid_recovered_before'
assert_contains rootfs/profile.d/mtd.sh 'parity_written'
assert_contains README.md 'QEMU'
assert_contains README.md 'MTD'
assert_contains README.md 'MTD_SMOKE=ubifs'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'TYPE_Q3N_NAND'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'TYPE_Q3N_NAND_PCI'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_PCI_VENDOR_ID'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_REG_GEOM0'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_REG_FAULT_ADDR_LO'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_REG_FAULT_CTRL'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_FAULT_INJECT_DATA_LOSS'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_REG_STAT_PAGE_READ_ERRORS'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_REG_STAT_FAULTS_INJECTED'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_DEFAULT_DATA_BLOCKS_PER_PLANE[[:space:]]+208'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_DEFAULT_PARITY_BLOCKS_PER_PLANE[[:space:]]+32'
for symbol in \
  Q3N_CMD_READ_PAGE_OOB \
  Q3N_CMD_PROGRAM_PAGE_OOB \
  Q3N_REG_OOB_LEN \
  Q3N_REG_STAT_FG_OPS \
  Q3N_REG_STAT_PARITY_READS \
  Q3N_REG_STAT_PARITY_WRITES \
  Q3N_REG_STAT_ORDER_ERRORS; do
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h "$symbol"
  assert_contains qemu/include/hw/mtd/q3n-nand.h "$symbol"
done
for symbol in \
  Q3N_CAP_PERSISTENT_MEDIA \
  Q3N_CAP_BAD_BLOCK_MARKER \
  Q3N_CMD_GET_BLOCK_STATUS \
  Q3N_CMD_MARK_BAD_BLOCK \
  Q3N_REG_BLOCK_STATUS \
  Q3N_REG_BLOCK_NEXT_PAGE; do
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h "$symbol"
  assert_contains qemu/include/hw/mtd/q3n-nand.h "$symbol"
done
assert_contains qemu/include/hw/mtd/q3n-media.h 'Q3N_BBM_GOOD.*0xff'
assert_contains qemu/include/hw/mtd/q3n-media.h 'Q3N_BBM_BAD.*0x00'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_erase_block'
assert_contains qemu/hw/mtd/q3n-nand.c 'next_prog_page'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_check_program_order'
assert_contains qemu/hw/mtd/q3n-media.c 'Q3N_BBM_GOOD'
assert_contains qemu/hw/mtd/q3n-nand.c 'stat_order_errors'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_inject_data_loss'
assert_contains qemu/hw/mtd/q3n-nand.c 'faults_injected'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_append_parity_record'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_recover_data_page'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_invalidate_group_parity'
assert_contains qemu/hw/mtd/q3n-pci.c 'TYPE_Q3N_NAND_PCI'
assert_contains qemu/hw/mtd/q3n-pci.c 'pci_register_bar'
assert_contains qemu/hw/mtd/q3n-pci.c 'DEFINE_PROP_DRIVE\("drive"'
assert_contains qemu/hw/mtd/q3n-media.c 'Q3NMEDIA'
assert_contains qemu/hw/mtd/q3n-media.c 'blk_truncate'
assert_contains qemu/hw/mtd/q3n-media.c 'blk_pread'
assert_contains qemu/hw/mtd/q3n-media.c 'blk_pwrite'
assert_contains qemu/hw/mtd/meson.build 'q3n-pci.c'
assert_contains qemu/hw/mtd/meson.build 'CONFIG_Q3N_NAND'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'MODULE_DEVICE_TABLE\(pci, qemu_3dnand_id_table\)'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_REG_ID'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd_device_register'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd_device_unregister'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_read'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_write'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_erase'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_finish_parity_work'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_cancel_block_parity'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'wait_event.*pending_parity'
for name in parity_pause_block parity_pause_enable parity_paused \
		pending_parity reserved_parity; do
	assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
		"debugfs_create_file.*$name"
done
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c 'q3n_sched_get_counts'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_READ_PAGE'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_PROGRAM_PAGE'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_ERASE_BLOCK'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'debugfs_create_dir'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_append_parity_locked'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_recover_page_locked'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_invalidate_block_parity'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'struct qemu_3dnand_data_block_meta'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'data_block_generation\[Q3N_RAID_LANES\]'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_parity_generation_valid'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'generation_updates'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'inject_data_loss'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_FAULT_INJECT_DATA_LOSS'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'raid_recovered'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'debugfs_create_file.*raid_failed'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'parity_stale'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'parity_sequence'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'faults_injected'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'devm_ioremap_resource'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_probe'
assert_contains linux/drivers/mtd/nand/raw/Kconfig.qemu_3dnand 'config MTD_NAND_QEMU_3DNAND'
assert_contains linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand 'qemu_3dnand.o'
assert_contains linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand 'qemu_3dnand_main.o qemu_3dnand_map.o'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_map.c 'div_u64_rem'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'q3n_validate_manifest'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'Q3N_STRIPE_UNPROTECTED'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n->parity_index = kvcalloc'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'kvfree\(q3n->parity_index\)'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n->parity_index = devm_kcalloc'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_sync = qemu_3dnand_mtd_sync'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_block_isbad = qemu_3dnand_mtd_block_isbad'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_block_markbad = qemu_3dnand_mtd_block_markbad'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'struct mutex mtd_lock'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n_sched_enqueue'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n_sched_try_start\(&parity->q3n->sched'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n_sched_requeue_p1\(&parity->q3n->sched'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'IS_ALIGNED\(instr->addr, mtd->erasesize\)'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c 'q3n_map_non_power_of_two_geometry_test'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c 'q3n_program_order_test'

printf 'ok: script structure verified\n'
