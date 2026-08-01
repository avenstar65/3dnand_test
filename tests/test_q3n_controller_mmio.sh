#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

. "$repo_root/scripts/lib/common.sh"

qemu_dir=$(selected_qemu_dir)
qemu_version=$(qemu_version_from_dir "$qemu_dir")
qemu_build_dir="$build_dir/qemu-$qemu_version"
if [ -x "$qemu_build_dir/qemu-system-x86_64-unsigned" ]; then
  qemu_bin="$qemu_build_dir/qemu-system-x86_64-unsigned"
elif [ -x "$qemu_build_dir/qemu-system-x86_64" ]; then
  qemu_bin="$qemu_build_dir/qemu-system-x86_64"
else
  die "未找到已构建的 QEMU: $qemu_build_dir/qemu-system-x86_64(-unsigned)，请先运行 ./scripts/shell.sh ./scripts/build-qemu.sh"
fi

case "$qemu_bin" in
  "$repo_root"/*) qemu_container_bin=/workspace/${qemu_bin#"$repo_root"/} ;;
  *) die "已选择的 QEMU 构建不在工作区内，无法映射到测试容器: $qemu_bin" ;;
esac

if [ "${Q3N_QTEST_PRINT_PATH:-}" = 1 ]; then
  printf '%s\n' "$qemu_container_bin"
  exit 0
fi

"$repo_root/scripts/shell.sh" python3 \
    /workspace/tests/test_q3n_controller_mmio.py --qemu "$qemu_container_bin"
printf 'ok: real Q3N controller MMIO multi-plane integration verified\n'
