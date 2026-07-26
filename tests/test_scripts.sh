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
  scripts/q3n-serial-smoke.sh \
  scripts/q3n-persistence-smoke.sh \
  scripts/gdb-kernel.sh \
  scripts/smoke-test.sh \
  configs/linux/qemu-x86_64-debug.fragment \
  configs/linux/qemu-x86_64-lean.fragment \
  configs/linux/mtd.fragment \
  configs/qemu/x86_64.env \
  rootfs/init \
  rootfs/helpers/mtd_badblock.c \
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
  qemu/hw/mtd/q3n-media-overlay.h \
  qemu/hw/mtd/q3n-nand.c \
  qemu/hw/mtd/q3n-pci.c \
  qemu/hw/mtd/meson.build \
  qemu/hw/mtd/Kconfig; do
  assert_file "$file"
done

assert_file tests/test_q3n_overlay.c
assert_file tests/test_q3n_overlay.sh
assert_file tests/test_q3n_controller.c

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
  scripts/q3n-serial-smoke.sh \
  scripts/q3n-persistence-smoke.sh \
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
assert_contains scripts/build-kernel.sh 'DEPMOD=true'
assert_contains scripts/build-kernel.sh 'depmod -b'
assert_contains scripts/build-kernel.sh 'build\*'
assert_contains scripts/fetch-linux.sh 'kernel.org'
assert_contains scripts/fetch-qemu.sh 'download.qemu.org'
assert_contains scripts/fetch-linux.sh 'KERNEL_BASE_URL'
assert_contains scripts/build-qemu.sh 'apply-qemu-overlay.sh'
assert_contains scripts/build-qemu.sh 'x86_64-softmmu'
assert_contains scripts/run-qemu.sh 'q3n-nand-pci'
assert_contains scripts/run-qemu.sh '--fresh-nand'
assert_contains scripts/run-qemu.sh '--nand-image'
assert_contains scripts/run-qemu.sh 'q3n-nand-pci,drive=q3n-media'
assert_contains scripts/q3n-serial-smoke.sh '--fresh-nand'
assert_contains scripts/q3n-serial-smoke.sh 'q3n serial smoke passed'
assert_contains scripts/q3n-serial-smoke.sh 'MTD smoke .*'
assert_contains scripts/q3n-serial-smoke.sh 'marker missing'
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
assert_contains rootfs/init 'ubifs.*mtd_ubifs'
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
assert_contains rootfs/profile.d/mtd.sh 'q3n-parity-stats'
for counter in foreground_ops parity_reads parity_writes \
		protected_stripes unprotected_stripes failed_stripes \
		max_pending_parity; do
	assert_contains rootfs/profile.d/mtd.sh "$counter"
done
assert_not_contains rootfs/profile.d/mtd.sh 'order_errors'
assert_contains rootfs/profile.d/mtd.sh 'inject_parity_program_fail'
assert_contains rootfs/profile.d/mtd.sh 'cancel_parity_program_fail'
assert_contains rootfs/profile.d/mtd.sh 'reset_controller'
assert_contains rootfs/profile.d/mtd.sh 'inject_invalid_program_fail'
assert_contains rootfs/profile.d/mtd.sh 'inject_parity_queue_fail'
assert_contains rootfs/profile.d/mtd.sh 'faults_after.*-eq.*faults_before.*\+ 1'
assert_contains rootfs/profile.d/mtd.sh 'failed_after.*-eq.*failed_before.*\+ 1'
assert_contains rootfs/profile.d/mtd.sh 'unprotected_after.*-eq.*unprotected_before.*\+ 1'
assert_contains rootfs/profile.d/mtd.sh 'protected_failed.*-eq.*protected_before_fail'
assert_contains rootfs/profile.d/mtd.sh 'parity_writes_after_fail.*-eq.*parity_writes_before_fail.*\+ 1'
assert_contains rootfs/profile.d/mtd.sh 'parity_writes_before_fail=\$\(cat.*parity_writes'
assert_contains rootfs/profile.d/mtd.sh 'pending_parity.*-eq 0'
assert_contains rootfs/profile.d/mtd.sh 'reserved_parity.*-eq 0'
assert_contains rootfs/profile.d/mtd.sh 'protected_later.*-eq.*protected_failed.*\+ 1'
assert_contains rootfs/profile.d/mtd.sh 'q3n-serial-page-%04d'
assert_contains rootfs/profile.d/mtd.sh 'parity_continuation_pause_enable'
assert_contains rootfs/profile.d/mtd.sh 'parity_continuation_paused'
assert_contains rootfs/profile.d/mtd.sh 'p0 continuation acceptance passed'
assert_contains rootfs/profile.d/mtd.sh 'p1 over p2 acceptance passed'
assert_contains rootfs/profile.d/mtd.sh 'while.*page.*-lt 8'
assert_contains rootfs/profile.d/mtd.sh 'cmp.*q3n-serial-page'
assert_contains rootfs/init 'q3n-serial-smoke.*mtd_q3n_serial_smoke'
assert_contains rootfs/profile.d/mtd.sh 'q3n-generation-smoke'
assert_contains rootfs/profile.d/mtd.sh 'q3n-cancel-barrier-smoke'
assert_contains rootfs/profile.d/mtd.sh 'q3n-markbad-smoke'
assert_contains rootfs/profile.d/mtd.sh 'q3n-persist-prepare'
assert_contains rootfs/profile.d/mtd.sh 'q3n-persist-verify'
assert_contains rootfs/profile.d/mtd.sh 'q3n-oob-bbm-test'
assert_not_contains rootfs/profile.d/mtd.sh 'error -EIO: failed to register MTD'
assert_contains rootfs/profile.d/mtd.sh 'q3n no-frontier stripe progress passed'
assert_not_contains rootfs/profile.d/mtd.sh 'tombstone'
assert_not_contains rootfs/init 'q3n-tail'
assert_contains rootfs/init 'q3n-persist-prepare.*mtd_q3n_persist_prepare'
assert_contains rootfs/init 'q3n-persist-verify.*mtd_q3n_persist_verify'
assert_contains scripts/q3n-persistence-smoke.sh '--fresh-nand'
assert_contains scripts/q3n-persistence-smoke.sh 'q3n-persist-prepare'
assert_contains scripts/q3n-persistence-smoke.sh 'q3n-persist-verify'
assert_not_contains scripts/q3n-persistence-smoke.sh 'q3n-tail'
assert_not_contains scripts/q3n-persistence-smoke.sh 'tail_reason'
assert_not_contains scripts/q3n-persistence-smoke.sh 'invalid-state'
assert_contains README.md 'QEMU persists raw NAND bytes and bitflip overlays\.'
assert_contains qemu/README.md 'QEMU persists raw NAND bytes and bitflip overlays\.'
assert_contains scripts/build-rootfs.sh 'mtd_badblock.c'
assert_contains rootfs/helpers/mtd_badblock.c 'MEMREADOOB64'
assert_contains rootfs/helpers/mtd_badblock.c 'MEMWRITEOOB64'
assert_contains rootfs/helpers/mtd_badblock.c 'MEMREAD'
assert_contains rootfs/helpers/mtd_badblock.c 'MEMWRITE'
assert_contains rootfs/helpers/mtd_badblock.c 'MTDFILEMODE'
assert_contains rootfs/helpers/mtd_badblock.c 'page-write'
assert_contains rootfs/helpers/mtd_badblock.c 'span-write'
assert_contains rootfs/helpers/mtd_badblock.c 'oob-read-unchecked'
assert_contains rootfs/helpers/mtd_badblock.c 'oob-read-uncorrectable'
assert_contains rootfs/helpers/mtd_badblock.c 'errno == EBADMSG'
assert_contains rootfs/helpers/mtd_badblock.c 'req\.length == 0'
assert_contains rootfs/profile.d/mtd.sh 'mtd_badblock oob-read'
assert_contains rootfs/profile.d/mtd.sh 'mtd_badblock oob-write'
assert_contains rootfs/profile.d/mtd.sh 'mtd_badblock oob-read-uncorrectable raw'
assert_contains rootfs/profile.d/mtd.sh 'OOB bounds rejection passed'
assert_contains rootfs/profile.d/mtd.sh 'kernel OOB bounds rejection passed'
assert_contains rootfs/profile.d/mtd.sh 'mtd_badblock oob-read-unchecked place'
assert_contains rootfs/profile.d/mtd.sh '"\$bounds_offset" 128 1'
assert_contains rootfs/profile.d/mtd.sh 'cross-page OOB passed'
assert_contains rootfs/profile.d/mtd.sh 'PLACE main\+OOB passed'
assert_contains rootfs/profile.d/mtd.sh 'RAW main\+OOB passed'
assert_contains rootfs/profile.d/mtd.sh 'good-page second OOB program rejected'
assert_contains rootfs/profile.d/mtd.sh 'q3n no-frontier stripe progress passed'
assert_contains rootfs/profile.d/mtd.sh 'cancel_writer_pid=\$!'
assert_contains rootfs/profile.d/mtd.sh 'wait "\$cancel_writer_pid"'
assert_contains rootfs/profile.d/mtd.sh 'dd if=/tmp/q3n-cancel\.bin of="\$mtd_dev" bs=16384 count=7 2>/tmp/q3n-cancel-dd\.err[[:space:]]*&[[:space:]]*$'
assert_not_contains rootfs/profile.d/mtd.sh 'kill "\$cancel_writer_pid"'
assert_contains rootfs/profile.d/mtd.sh 'protected_after'
assert_contains rootfs/profile.d/mtd.sh 'failed_after'
assert_contains rootfs/profile.d/mtd.sh 'inject_parity_program_fail'
assert_contains README.md 'QEMU'
assert_contains README.md 'MTD'
assert_contains README.md 'MTD_SMOKE=ubifs'
assert_contains README.md 'q3n-persistence-smoke.sh'
assert_contains README.md '--fresh-nand'
assert_contains README.md 'q3n-serial-smoke.sh'
assert_contains qemu/README.md 'Q3NMEDIA'
assert_contains qemu/README.md 'OOB byte 0'
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
header=qemu/include/hw/mtd/q3n-nand.h
assert_contains "$header" 'Q3N_PHYSICAL_OOB_HEAD_OFFSET[[:space:]]+Q3N_PAGE_SIZE'
assert_contains "$header" 'Q3N_PHYSICAL_OOB_HEAD_SIZE[[:space:]]+1U'
assert_contains "$header" 'Q3N_PHYSICAL_LDPC_OFFSET'
assert_contains "$header" 'Q3N_PHYSICAL_OOB_TAIL_OFFSET'
assert_contains "$header" 'Q3N_PHYSICAL_OOB_TAIL_SIZE'
assert_contains "$header" 'Q3N_PHYSICAL_PAGE_SIZE'
assert_not_contains qemu/include/hw/mtd/q3n-media.h 'q3n_media_mark_bad'
assert_not_contains qemu/hw/mtd/q3n-media.c 'q3n_media_mark_bad'
assert_not_contains qemu/hw/mtd/q3n-media.c 'q3n_media_write_bbm'
assert_contains qemu/include/hw/mtd/q3n-media.h 'q3n_media_read_logical_oob'
assert_contains qemu/include/hw/mtd/q3n-media.h 'q3n_media_program_logical_oob'
for symbol in \
  Q3N_CMD_READ_PAGE_OOB \
  Q3N_CMD_PROGRAM_PAGE_OOB \
  Q3N_REG_OOB_LEN \
  Q3N_REG_STAT_FG_OPS \
  Q3N_REG_STAT_PARITY_READS \
  Q3N_REG_STAT_PARITY_WRITES \
  Q3N_FAULT_FAIL_NEXT_PROGRAM; do
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h "$symbol"
  assert_contains qemu/include/hw/mtd/q3n-nand.h "$symbol"
done
for symbol in \
  Q3N_CAP_PERSISTENT_MEDIA \
  Q3N_CAP_BAD_BLOCK_MARKER \
  Q3N_CMD_GET_BLOCK_STATUS \
  Q3N_REG_BLOCK_STATUS; do
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h "$symbol"
  assert_contains qemu/include/hw/mtd/q3n-nand.h "$symbol"
done
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h \
  'Q3N_CMD_MARK_BAD_BLOCK'
assert_not_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_CMD_MARK_BAD_BLOCK'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'Q3N_CMD_MARK_BAD_BLOCK'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_cmd_mark_bad_block'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_media_mark_bad'
for header in \
  linux/drivers/mtd/nand/raw/qemu_3dnand.h \
  qemu/include/hw/mtd/q3n-nand.h; do
  assert_not_contains "$header" 'Q3N_REG_BLOCK_NEXT_PAGE'
  assert_not_contains "$header" 'Q3N_REG_STAT_ORDER_ERRORS'
done
for symbol in \
  Q3N_PHYSICAL_OOB_SIZE Q3N_LOGICAL_OOB_SIZE Q3N_BBM_OOB_OFFSET \
  Q3N_LDPC_OOB_OFFSET Q3N_LDPC_BYTES_PER_STEP Q3N_LDPC_STEPS \
  Q3N_METADATA_OOB_OFFSET Q3N_REG_ECC_GEOM0 Q3N_REG_ECC_GEOM1 \
  Q3N_REG_ECC_STATUS Q3N_REG_ECC_MAX_BITFLIPS \
  Q3N_REG_ECC_CORRECTED_BITS Q3N_REG_ECC_FAILED_STEP \
  Q3N_REG_FAULT_STEP Q3N_REG_FAULT_FIRST_BIT \
  Q3N_REG_FAULT_COUNT Q3N_REG_FAULT_REGION \
  Q3N_FAULT_INJECT_BITFLIPS; do
  assert_contains qemu/include/hw/mtd/q3n-nand.h "$symbol"
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h "$symbol"
done
assert_contains qemu/include/hw/mtd/q3n-media.h 'Q3N_BBM_GOOD.*0xff'
assert_contains qemu/include/hw/mtd/q3n-media.h 'Q3N_BBM_BAD.*0x00'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_erase_block'
assert_contains qemu/hw/mtd/q3n-media.c 'Q3N_BBM_GOOD'
for source in \
  qemu/hw/mtd/q3n-nand.c \
  qemu/hw/mtd/q3n-media.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_map.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c; do
  assert_not_contains "$source" 'next_prog_page'
  assert_not_contains "$source" 'tombstone'
done
assert_contains qemu/hw/mtd/q3n-nand.c 'fail_next_program'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_disarm_program_fault'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_disarm_program_fault\(s\);'
assert_contains qemu/hw/mtd/q3n-nand.c 'stats\.fg_ops\+\+'
assert_contains qemu/hw/mtd/q3n-nand.c 'stats\.parity_reads\+\+'
assert_contains qemu/hw/mtd/q3n-nand.c 'stats\.parity_writes\+\+'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_inject_data_loss'
assert_contains qemu/hw/mtd/q3n-nand.c 'faults_injected'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_logical_to_physical_oob'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_physical_to_logical_oob'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_media_read_logical_oob'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_media_program_logical_oob'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_oob_transfer_valid'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_generate_ldpc_step'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_decode_ldpc'
assert_contains qemu/hw/mtd/q3n-nand.c 'ecc_max_bitflips'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_append_parity_record'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_recover_data_page'
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_invalidate_group_parity'
assert_contains qemu/hw/mtd/q3n-pci.c 'TYPE_Q3N_NAND_PCI'
assert_contains qemu/hw/mtd/q3n-pci.c 'pci_register_bar'
assert_contains qemu/hw/mtd/q3n-pci.c 'DEFINE_PROP_DRIVE\("drive"'
assert_contains qemu/hw/mtd/q3n-media.c 'Q3NMEDIA'
assert_contains qemu/hw/mtd/q3n-media.c 'Q3N_MEDIA_VERSION[[:space:]]+2'
assert_contains qemu/hw/mtd/q3n-media.c 'physical_oob_size'
assert_contains qemu/hw/mtd/q3n-media.c 'overlay_slots_offset'
assert_contains qemu/hw/mtd/q3n-media.c 'q3n_media_inject_bitflips'
assert_contains qemu/hw/mtd/q3n-media-overlay.h 'Q3N_MEDIA_OVERLAY_STRIDE'
assert_contains qemu/hw/mtd/q3n-media.c 'Q3N_PHYSICAL_OOB_TAIL_OFFSET'
assert_contains qemu/hw/mtd/q3n-media.c 'blk_truncate'
assert_contains qemu/hw/mtd/q3n-media.c 'blk_pread'
assert_contains qemu/hw/mtd/q3n-media.c 'blk_pwrite'
assert_contains qemu/hw/mtd/q3n-media.c \
  '\*status = marker != Q3N_BBM_GOOD \? Q3N_BLOCK_STATUS_BAD : 0'
assert_contains qemu/hw/mtd/meson.build 'q3n-pci.c'
assert_contains qemu/hw/mtd/meson.build 'CONFIG_Q3N_NAND'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'MODULE_DEVICE_TABLE\(pci, qemu_3dnand_id_table\)'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_REG_ID'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd_device_register'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd_device_unregister'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_read'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_write'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_read_oob'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_write_oob'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->oobsize = Q3N_LOGICAL_OOB_SIZE'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_read_oob = qemu_3dnand_mtd_read_oob'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_write_oob = qemu_3dnand_mtd_write_oob'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_read = qemu_3dnand_mtd_read'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_write = qemu_3dnand_mtd_write'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_erase'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_finish_parity_work'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_cancel_block_parity'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'wait_event.*pending_parity'
for name in parity_pause_block parity_pause_enable parity_paused \
		pending_parity reserved_parity foreground_ops parity_reads \
		parity_writes protected_stripes unprotected_stripes \
		failed_stripes max_pending_parity inject_parity_program_fail; do
	assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
		"debugfs_create_file.*$name"
done
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c 'q3n_sched_get_counts'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_READ_PAGE'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_PROGRAM_PAGE'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_ERASE_BLOCK'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'debugfs_create_dir'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_commit_parity_locked'
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
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_restore_media_locked'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_GET_BLOCK_STATUS'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_MARK_BAD_BLOCK'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CAP_BAD_BLOCK_MARKER'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'q3n_validate_manifest'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'q3n_pack_data_oob'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'q3n_unpack_data_oob'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'q3n_pack_parity_oob'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'q3n_unpack_parity_oob'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'manifest->data_crc\[lane\] = cpu_to_le32\(stripe->data_crc\[lane\]\)'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'Q3N_STRIPE_UNPROTECTED'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n->parity_index = kvcalloc'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'kvfree\(q3n->parity_index\)'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n->parity_index = devm_kcalloc'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_sync = qemu_3dnand_mtd_sync'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_block_isbad = qemu_3dnand_mtd_block_isbad'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'mtd->_block_markbad = qemu_3dnand_mtd_block_markbad'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'struct mutex mtd_lock'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n_sched_enqueue'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n_sched_try_start_seq\(&parity->q3n->sched'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n_sched_wait_for_change'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n_sched_notify'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'q3n_sched_requeue_p1\(&parity->q3n->sched'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'queue_work\(parity->q3n->parity_wq, &parity->work\)'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'cond_resched\(\)'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'atomic64_t protected_stripes'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'atomic64_t unprotected_stripes'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'atomic64_t failed_stripes'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'atomic64_inc\(&q3n->protected_stripes\)'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'atomic64_read\(&q3n->protected_stripes\)'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'parity_continuation_pause_enable'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'parity_continuation_paused'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'alloc_workqueue\("q3n-parity"'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_MAX_PENDING_PARITY'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'done \+= q3n->page_size;'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'IS_ALIGNED\(instr->addr, mtd->erasesize\)'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c 'q3n_map_non_power_of_two_geometry_test'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c 'q3n_scheduler_does_not_gate_program_page_test'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c 'q3n_scheduler_tracks_max_pending_test'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c 'q3n_data_oob_round_trip_preserves_bbm_test'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c 'q3n_parity_oob_round_trip_and_crc_validation_test'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c \
	'q3n_ecc_accumulate_uses_max_and_sums_corrected_test'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c \
	'q3n_ecc_accumulate_defers_failure_accounting_once_test'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
	'bool uncorrectable'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
	'q3n_ecc_result_to_mtd_ret'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'mtd->ecc_step_size = Q3N_ECC_STEP_SIZE'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'mtd->ecc_strength = Q3N_ECC_STRENGTH'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'mtd->bitflip_threshold = Q3N_ECC_STRENGTH'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'background_ecc_corrected_bits'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'raid_source_corrected_bits'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'Q3N_REG_ECC_GEOM0'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'Q3N_REG_ECC_GEOM1'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'ecc_stats\.failed'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_read_phys_page_oob_locked'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_program_phys_page_oob_locked'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_READ_PAGE_OOB'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_PROGRAM_PAGE_OOB'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'tombstone'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'tombstone'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h 'TOMBSTONE'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'qemu_3dnand_account_queue_failure'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'ecc\.uncorrectable'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
	'le32_to_cpu\(meta\.data_crc\) != crc'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'get_unaligned_le32'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'put_unaligned_le32'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'u32 \*oob_words'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'u32 \*data_words'

controller_tmp=$(mktemp -d "${TMPDIR:-/tmp}/q3n-controller.XXXXXX")
trap 'rm -rf "$controller_tmp"' EXIT HUP INT TERM
sed -n '/Q3N_CONTROLLER_HELPERS_BEGIN/,/Q3N_CONTROLLER_HELPERS_END/p' \
  "$repo_root/qemu/hw/mtd/q3n-nand.c" > \
  "$controller_tmp/q3n-controller-helpers.inc"
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
  -I "$controller_tmp" \
  "$repo_root/tests/test_q3n_controller.c" \
  -o "$controller_tmp/test_q3n_controller"
"$controller_tmp/test_q3n_controller"

printf 'ok: script structure verified\n'
