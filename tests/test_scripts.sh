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
  scripts/configure-kernel.sh \
  scripts/build-kernel.sh \
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
  drivers/mtd_demo/mtd_demo.c; do
  assert_file "$file"
done

for file in \
  scripts/build-image.sh \
  scripts/shell.sh \
  scripts/fetch-linux.sh \
  scripts/configure-kernel.sh \
  scripts/build-kernel.sh \
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
assert_contains Dockerfile 'busybox-static:amd64'
assert_contains .dockerignore '^work/'
assert_contains scripts/build-image.sh 'BASE_IMAGE'
assert_contains scripts/configure-kernel.sh 'CROSS_COMPILE'
assert_contains scripts/configure-kernel.sh 'merge_config.sh'
assert_contains scripts/configure-kernel.sh '"\$merge" -m'
assert_contains scripts/build-kernel.sh 'CROSS_COMPILE'
assert_contains scripts/fetch-linux.sh 'kernel.org'
assert_contains scripts/fetch-linux.sh 'KERNEL_BASE_URL'
assert_contains scripts/fetch-linux.sh 'delay-directory-restore'
assert_contains scripts/fetch-linux.sh 'redownload'
assert_contains scripts/fetch-linux.sh 'continue-at'
assert_contains scripts/fetch-linux.sh 'rsync'
assert_contains scripts/fetch-linux.sh 'extract_tmp'
assert_contains scripts/build-rootfs.sh 'ROOTFS_BUSYBOX'
assert_contains scripts/build-rootfs.sh 'x86-64'
assert_contains rootfs/init 'poweroff -f'
assert_contains scripts/run-qemu.sh '-s -S'
assert_contains configs/linux/mtd.fragment 'CONFIG_MTD_NAND_NANDSIM'
assert_contains configs/linux/qemu-x86_64-lean.fragment 'CONFIG_DRM is not set'
assert_contains scripts/configure-kernel.sh 'qemu-x86_64-lean.fragment'
assert_contains drivers/mtd_demo/mtd_demo.c 'get_mtd_device'
assert_contains drivers/mtd_demo/mtd_demo.c 'put_mtd_device'
assert_contains README.md 'QEMU'
assert_contains README.md 'MTD'

printf 'ok: script structure verified\n'
