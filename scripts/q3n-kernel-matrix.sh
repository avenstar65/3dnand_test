#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

if [ "$(uname -s)" = Darwin ]; then
	exec "$repo_root/scripts/shell.sh" scripts/q3n-kernel-matrix.sh "$@"
fi

[ "$#" -eq 0 ] || die "用法: $0"
need_cmd make
mkdirs

linux_dir=$(selected_linux_dir)
version=$(kernel_version_from_dir "$linux_dir")
kernel_arch=${KERNEL_ARCH:-x86_64}
cross_compile=${CROSS_COMPILE:-x86_64-linux-gnu-}
seed_vmlinux=
seed_dir="$work_dir/build-matrix/kernel-abi-seed"

for mode in identity page-raid-2 page-raid-4 page-raid-8 multiplane; do
	case "$mode" in
		identity) expected_mode=identity ;;
		page-raid-2) expected_mode=raid2 ;;
		page-raid-4) expected_mode=raid4 ;;
		page-raid-8) expected_mode=raid8 ;;
		multiplane) expected_mode=multiplane ;;
	esac

	profile_build="$work_dir/build-matrix/$mode"
	build_output="$profile_build/linux-$version"
	info "构建 Q3N storage mode: $mode"
	BUILD_DIR="$profile_build" \
		"$repo_root/scripts/configure-kernel.sh" --q3n-mode "$mode"
	if [ "$mode" = identity ]; then
		make -C "$linux_dir" O="$build_output" ARCH="$kernel_arch" \
			CROSS_COMPILE="$cross_compile" vmlinux
		seed_vmlinux="$build_output/vmlinux.o"
		[ -f "$seed_vmlinux" ] || die "未生成 Q3N matrix ABI seed: $seed_vmlinux"
		make -C "$linux_dir" O="$build_output" ARCH="$kernel_arch" \
			CROSS_COMPILE="$cross_compile" modules
		mkdir -p "$seed_dir"
		cp "$seed_vmlinux" "$seed_dir/vmlinux.o"
		seed_vmlinux="$seed_dir/vmlinux.o"
	else
		[ -n "$seed_vmlinux" ] || die "Q3N matrix ABI seed is unavailable"
		cp "$seed_vmlinux" "$build_output/vmlinux.o"
	fi
	if [ "$mode" != identity ]; then
		make -C "$linux_dir" O="$build_output" ARCH="$kernel_arch" \
			CROSS_COMPILE="$cross_compile" modules
	fi
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$build_output" \
		Q3N_EXPECTED_MODE="$expected_mode" \
		sh "$repo_root/tests/test_q3n_nand_core_contract.sh"
done

info "Q3N storage-mode build matrix completed"
