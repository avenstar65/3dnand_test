#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

# QEMU is built by the Linux toolchain container.  On macOS, transparently
# execute this script in that same container instead of trying to run an ELF
# binary on the host.
if [ "$(uname -s)" = "Darwin" ]; then
  exec "$repo_root/scripts/shell.sh" ./scripts/run-qemu.sh "$@"
fi

debug=0
extra_append=

usage() {
  cat <<'USAGE'
用法: scripts/run-qemu.sh [--debug] [--append "额外内核参数"]

--debug  添加 -s -S，让 QEMU 等待 GDB 连接。
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --debug) debug=1 ;;
    --append)
      shift
      [ "$#" -gt 0 ] || die "--append 需要参数"
      extra_append=$1
      ;;
    -h|--help) usage; exit 0 ;;
    *) die "未知参数: $1" ;;
  esac
  shift
done

. "$repo_root/configs/qemu/x86_64.env"

repo_qemu="$build_dir/qemu-11.0.2/qemu-system-x86_64-unsigned"
if [ ! -x "$repo_qemu" ]; then
  repo_qemu="$build_dir/qemu-11.0.2/qemu-system-x86_64"
fi
if [ "${QEMU_BIN:-}" = "qemu-system-x86_64" ] && [ -x "$repo_qemu" ]; then
  QEMU_BIN="$repo_qemu"
fi

need_cmd "$QEMU_BIN"
mkdirs

linux_dir=$(selected_linux_dir)
version=$(kernel_version_from_dir "$linux_dir")
out_dir="$build_dir/linux-$version"
bzimage="$out_dir/arch/x86/boot/bzImage"
initramfs="$rootfs_build_dir/initramfs.cpio.gz"

[ -f "$bzimage" ] || die "缺少 bzImage，请先运行 ./scripts/build-kernel.sh"
[ -f "$initramfs" ] || die "缺少 initramfs，请先运行 ./scripts/build-rootfs.sh"

debug_args=
if [ "$debug" -eq 1 ]; then
  debug_args="-s -S"
  info "调试模式已开启: GDB 连接 localhost:1234"
fi

exec "$QEMU_BIN" \
  -machine "$QEMU_MACHINE" \
  -cpu "$QEMU_CPU" \
  -m "$QEMU_MEMORY" \
  -smp "$QEMU_SMP" \
  -kernel "$bzimage" \
  -initrd "$initramfs" \
  -append "$QEMU_APPEND $extra_append" \
  -device q3n-nand-pci \
  -nographic \
  -no-reboot \
  $debug_args
