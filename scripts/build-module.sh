#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd make
mkdirs

linux_dir=$(selected_linux_dir)
version=$(kernel_version_from_dir "$linux_dir")
out_dir="$build_dir/linux-$version"
module_dir="$repo_root/drivers/mtd_demo"

[ -d "$out_dir" ] || die "缺少内核构建目录: $out_dir"

kernel_release=$(make -s -C "$linux_dir" O="$out_dir" kernelrelease)
module_install_dir="$out_dir/modules/lib/modules/$kernel_release/extra"

info "编译样例 MTD 模块"
make -C "$linux_dir" O="$out_dir" M="$module_dir" modules

[ -f "$module_dir/mtd_demo.ko" ] || die "未生成 mtd_demo.ko"

mkdir -p "$module_install_dir"
cp "$module_dir/mtd_demo.ko" "$module_install_dir/"

if command -v depmod >/dev/null 2>&1; then
  depmod -b "$out_dir/modules" "$kernel_release" || true
fi

info "模块完成: $module_install_dir/mtd_demo.ko"
