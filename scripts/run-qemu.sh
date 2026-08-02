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
nand_image=
nand_mode=identity
fresh_nand=0

usage() {
  cat <<'USAGE'
用法: scripts/run-qemu.sh [--debug] [--append "额外内核参数"]
                           [--nand-mode identity|page-raid|multiplane]
                           [--nand-image 路径] [--fresh-nand]

--debug  添加 -s -S，让 QEMU 等待 GDB 连接。
--nand-mode  选择 NAND 介质布局；默认 identity。
--nand-image  指定持久化物理 NAND 镜像，默认 work/media/q3n-nand.raw。
--fresh-nand  启动前删除指定镜像，创建全新的擦除态 NAND。
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
    --nand-mode)
      shift
      [ "$#" -gt 0 ] || die "--nand-mode 需要参数"
      nand_mode=$1
      ;;
    --nand-image)
      shift
      [ "$#" -gt 0 ] || die "--nand-image 需要参数"
      nand_image=$1
      ;;
    --fresh-nand) fresh_nand=1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "未知参数: $1" ;;
  esac
  shift
done

case "$nand_mode" in
  identity|page-raid|multiplane) ;;
  *) die "未知 NAND mode: $nand_mode (可选值: identity, page-raid, multiplane)" ;;
esac

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

case "$nand_mode" in
  identity)
    mode_symbol=CONFIG_MTD_NAND_QEMU_3DNAND_IDENTITY
    default_image=q3n-nand.raw
    ;;
  page-raid)
    mode_symbol=CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID
    default_image=q3n-nand.raw
    ;;
  multiplane)
    mode_symbol=CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE
    default_image=q3n-nand-multiplane.raw
    ;;
esac

kernel_config="$out_dir/.config"
[ -f "$kernel_config" ] || die "缺少内核配置: $kernel_config"
grep -Fqx "$mode_symbol=y" "$kernel_config" ||
  die "NAND mode '$nand_mode' 要求已构建 $mode_symbol=y"

if [ -z "$nand_image" ]; then
  nand_image="$work_dir/media/$default_image"
elif [ "${nand_image#/}" = "$nand_image" ]; then
  nand_image="$repo_root/$nand_image"
fi
image_dir=$(dirname -- "$nand_image")
image_base=$(basename -- "$nand_image")
mkdir -p "$image_dir"
image_dir=$(CDPATH= cd -- "$image_dir" && pwd -P)
nand_image="$image_dir/$image_base"
if [ "$fresh_nand" -eq 1 ]; then
  rm -f -- "$nand_image"
  info "已重置物理 NAND 镜像: $nand_image"
fi
[ -e "$nand_image" ] || : >"$nand_image"

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
  -blockdev "driver=file,filename=$nand_image,node-name=q3n-file" \
  -blockdev driver=raw,file=q3n-file,node-name=q3n-media \
  -device q3n-nand-pci,drive=q3n-media \
  -nographic \
  -no-reboot \
  $debug_args
