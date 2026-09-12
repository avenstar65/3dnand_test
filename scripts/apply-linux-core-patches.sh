#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

linux_dir=${1:-$(selected_linux_dir)}
patch_dir="$repo_root/linux/patches"
stage=$(mktemp -d "${TMPDIR:-/tmp}/q3n-linux-patches.XXXXXX")
forward="$stage/forward"
reverse="$stage/reverse"
trap 'rm -rf "$stage"' EXIT HUP INT TERM

files='drivers/mtd/nand/raw/internals.h
drivers/mtd/nand/raw/nand_base.c
drivers/mtd/nand/raw/nand_bbt.c
include/linux/mtd/rawnand.h'

file_hash() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	else
		shasum -a 256 "$1" | awk '{print $1}'
	fi
}

expected_hash() {
	case "$1" in
		drivers/mtd/nand/raw/internals.h) printf '%s\n' 6d2298a600c23b28c2d39cffc61a7e1913d2d12b04948435aeb81565a9376c37 ;;
		drivers/mtd/nand/raw/nand_base.c) printf '%s\n' 33ae4af428e1630014002a26b66e82c915a9f50cab27e832d7d3b5782b0a5d4c ;;
		drivers/mtd/nand/raw/nand_bbt.c) printf '%s\n' b726825d2a03889f04e08f3b5f1ebe7ceeadf5d009a728cb25e659adaf659f56 ;;
		include/linux/mtd/rawnand.h) printf '%s\n' 8e57635975d936181aefd2945b81babf4f16c7467a6039c3666cf471cf18c1e8 ;;
		*) return 1 ;;
	esac
}

for file in $files; do
	[ -f "$linux_dir/$file" ] || die "内核源码缺少文件: $file"
	mkdir -p "$forward/$(dirname "$file")" "$reverse/$(dirname "$file")"
	cp "$linux_dir/$file" "$forward/$file"
	cp "$linux_dir/$file" "$reverse/$file"
done

forward_ok=1
for patch in "$patch_dir"/*.patch; do
	[ -f "$patch" ] || die "没有找到 Linux core patch"
	git -C "$forward" apply --check "$patch" >/dev/null 2>&1 || {
		forward_ok=0
		break
	}
	git -C "$forward" apply "$patch"
done

if [ "$forward_ok" -eq 0 ]; then
	for file in $files; do
		[ "$(file_hash "$linux_dir/$file")" = "$(expected_hash "$file")" ] ||
			die "已应用的内核文件被修改，拒绝覆盖: $file"
	done
	set -- "$patch_dir"/*.patch
	reverse_list=
	for patch do reverse_list="$patch $reverse_list"; done
	for patch in $reverse_list; do
		git -C "$reverse" apply --reverse --check "$patch" >/dev/null 2>&1 ||
			die "内核补丁与源码不匹配或处于部分应用状态: $(basename "$patch")"
		git -C "$reverse" apply --reverse "$patch"
	done
	info "Linux core patches 已应用"
	exit 0
fi

for file in $files; do
	cp "$forward/$file" "$linux_dir/$file"
done
info "完成 Linux core patches: $linux_dir"
