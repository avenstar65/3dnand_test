#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd make
mkdirs

linux_dir=$(selected_linux_dir)
version=$(kernel_version_from_dir "$linux_dir")
out_dir="$build_dir/linux-$version"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}

[ -f "$out_dir/.config" ] || die "缺少 .config，请先运行 ./scripts/configure-kernel.sh"

info "编译 Linux $version，jobs=$jobs"
make -C "$linux_dir" O="$out_dir" -j"$jobs"
make -C "$linux_dir" O="$out_dir" modules_install INSTALL_MOD_PATH="$out_dir/modules"

[ -f "$out_dir/arch/x86/boot/bzImage" ] || die "未生成 bzImage"
[ -f "$out_dir/vmlinux" ] || die "未生成 vmlinux"

info "内核编译完成: $out_dir"

