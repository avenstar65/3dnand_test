#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd make
mkdirs

linux_dir=$(selected_linux_dir)
version=$(kernel_version_from_dir "$linux_dir")
out_dir="$build_dir/linux-$version"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}
kernel_arch=${KERNEL_ARCH:-x86_64}
cross_compile=${CROSS_COMPILE:-x86_64-linux-gnu-}

[ -f "$out_dir/.config" ] || die "缺少 .config，请先运行 ./scripts/configure-kernel.sh"

info "编译 Linux $version，jobs=$jobs"
make -C "$linux_dir" O="$out_dir" ARCH="$kernel_arch" CROSS_COMPILE="$cross_compile" -j"$jobs"
kernel_release=$(make -s -C "$linux_dir" O="$out_dir" ARCH="$kernel_arch" \
  CROSS_COMPILE="$cross_compile" kernelrelease)
module_tree="$out_dir/modules/lib/modules/$kernel_release"
make -C "$linux_dir" O="$out_dir" ARCH="$kernel_arch" \
  CROSS_COMPILE="$cross_compile" modules_install \
  INSTALL_MOD_PATH="$out_dir/modules" DEPMOD=true

# Docker Desktop/FileProvider can duplicate the kernel's `build` symlink as
# `build 2`, `build 3`, ... on a bind mount. depmod follows those links back
# into the output tree (and its modules directory), recursively opening FDs.
# Generate dependency metadata without build-tree links, then restore the
# canonical link required for out-of-tree module builds.
find "$module_tree" -maxdepth 1 -type l -name 'build*' -exec rm -f {} +
if command -v depmod >/dev/null 2>&1; then
  depmod -b "$out_dir/modules" "$kernel_release"
fi
ln -sfn "$out_dir" "$module_tree/build"

[ -f "$out_dir/arch/x86/boot/bzImage" ] || die "未生成 bzImage"
[ -f "$out_dir/vmlinux" ] || die "未生成 vmlinux"

info "内核编译完成: $out_dir"
