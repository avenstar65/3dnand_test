#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

usage()
{
	printf '用法: %s [--q3n-mode identity|page-raid-2|page-raid-4|page-raid-8|multiplane]\n' "$0"
}

q3n_mode=identity
while [ "$#" -gt 0 ]; do
	case "$1" in
		--q3n-mode)
			[ "$#" -ge 2 ] || die "--q3n-mode 需要一个值"
			q3n_mode=$2
			shift 2
			;;
		*)
			usage >&2
			die "未知参数: $1"
			;;
	esac
done

case "$q3n_mode" in
	identity) q3n_fragment="$repo_root/configs/linux/q3n-identity.fragment" ;;
	page-raid-2) q3n_fragment="$repo_root/configs/linux/q3n-page-raid-2.fragment" ;;
	page-raid-4) q3n_fragment="$repo_root/configs/linux/q3n-page-raid-4.fragment" ;;
	page-raid-8) q3n_fragment="$repo_root/configs/linux/q3n-page-raid-8.fragment" ;;
	multiplane) q3n_fragment="$repo_root/configs/linux/q3n-multiplane.fragment" ;;
	*)
		usage >&2
		die "未知 Q3N storage mode: $q3n_mode (可选值: identity, page-raid-2, page-raid-4, page-raid-8, multiplane)"
		;;
esac

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
info "Q3N storage mode: $q3n_mode"
make -C "$linux_dir" O="$out_dir" ARCH="$kernel_arch" CROSS_COMPILE="$cross_compile" x86_64_defconfig

merge="$linux_dir/scripts/kconfig/merge_config.sh"
[ -x "$merge" ] || die "找不到 merge_config.sh: $merge"

"$merge" -m -O "$out_dir" \
  "$out_dir/.config" \
  "$repo_root/configs/linux/qemu-x86_64-debug.fragment" \
  "$repo_root/configs/linux/qemu-x86_64-lean.fragment" \
  "$repo_root/configs/linux/mtd.fragment" \
  "$q3n_fragment"

make -C "$linux_dir" O="$out_dir" ARCH="$kernel_arch" CROSS_COMPILE="$cross_compile" olddefconfig

info "内核配置完成: $out_dir/.config"
