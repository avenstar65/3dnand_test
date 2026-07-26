#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd make
mkdirs

linux_dir=$(selected_linux_dir)
version=$(kernel_version_from_dir "$linux_dir")
out_dir="$build_dir/linux-$version"
kernel_arch=${KERNEL_ARCH:-x86_64}
cross_compile=${CROSS_COMPILE:-x86_64-linux-gnu-}

"$repo_root/scripts/apply-linux-overlay.sh"

mkdir -p "$out_dir"

info "配置 Linux $version，输出目录: $out_dir"
make -C "$linux_dir" O="$out_dir" ARCH="$kernel_arch" CROSS_COMPILE="$cross_compile" x86_64_defconfig

merge="$linux_dir/scripts/kconfig/merge_config.sh"
[ -x "$merge" ] || die "找不到 merge_config.sh: $merge"

"$merge" -m -O "$out_dir" \
  "$out_dir/.config" \
  "$repo_root/configs/linux/qemu-x86_64-debug.fragment" \
  "$repo_root/configs/linux/qemu-x86_64-lean.fragment" \
  "$repo_root/configs/linux/mtd.fragment"

make -C "$linux_dir" O="$out_dir" ARCH="$kernel_arch" CROSS_COMPILE="$cross_compile" olddefconfig

info "内核配置完成: $out_dir/.config"
