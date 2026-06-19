#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

case "$(basename "$script_dir")" in
  lib) repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd) ;;
esac

work_dir=${WORK_DIR:-"$repo_root/work"}
linux_work_dir=${LINUX_WORK_DIR:-"$work_dir/linux"}
build_dir=${BUILD_DIR:-"$work_dir/build"}
downloads_dir=${DOWNLOADS_DIR:-"$work_dir/downloads"}
rootfs_build_dir=${ROOTFS_BUILD_DIR:-"$work_dir/rootfs"}
image_name=${IMAGE_NAME:-linux-mtd-qemu-dev:latest}

die() {
  printf '错误: %s\n' "$*" >&2
  exit 1
}

info() {
  printf '==> %s\n' "$*"
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"
}

mkdirs() {
  mkdir -p "$work_dir" "$linux_work_dir" "$build_dir" "$downloads_dir" "$rootfs_build_dir"
}

latest_linux_dir() {
  find "$linux_work_dir" -maxdepth 1 -type d -name 'linux-*' 2>/dev/null | sort -V | tail -n 1
}

selected_linux_dir() {
  if [ "${LINUX_DIR:-}" ]; then
    printf '%s\n' "$LINUX_DIR"
    return 0
  fi

  latest=$(latest_linux_dir)
  [ -n "$latest" ] || die "未找到内核源码，请先运行 ./scripts/shell.sh ./scripts/fetch-linux.sh"
  printf '%s\n' "$latest"
}

kernel_version_from_dir() {
  basename "$1" | sed 's/^linux-//'
}

