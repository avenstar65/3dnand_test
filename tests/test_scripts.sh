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
  qemu/README.md \
  qemu/include/hw/mtd/q3n-nand.h \
  qemu/hw/mtd/q3n-nand.c \
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
assert_contains scripts/configure-kernel.sh '"\$merge" -m'
assert_contains scripts/build-kernel.sh 'CROSS_COMPILE'
assert_contains scripts/fetch-linux.sh 'kernel.org'
assert_contains scripts/fetch-qemu.sh 'download.qemu.org'
assert_contains scripts/fetch-linux.sh 'KERNEL_BASE_URL'
assert_contains scripts/build-qemu.sh 'apply-qemu-overlay.sh'
assert_contains scripts/build-qemu.sh 'x86_64-softmmu'
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
assert_contains README.md 'QEMU'
assert_contains README.md 'MTD'
assert_contains README.md 'MTD_SMOKE=ubifs'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'TYPE_Q3N_NAND'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_DEFAULT_DATA_BLOCKS_PER_PLANE[[:space:]]+208'
assert_contains qemu/include/hw/mtd/q3n-nand.h 'Q3N_DEFAULT_PARITY_BLOCKS_PER_PLANE[[:space:]]+32'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_append_parity_record'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_erase_data_block'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_recover_data_page'
assert_contains qemu/hw/mtd/meson.build 'CONFIG_Q3N_NAND'

printf 'ok: script structure verified\n'
