#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
patch_dir=${Q3N_LINUX_PATCH_DIR:-"$repo_root/linux/patches"}
linux_dir=${1:-}

die()
{
	printf '错误: %s\n' "$*" >&2
	exit 1
}

[ -n "$linux_dir" ] ||
	die "用法: $0 <linux-source-dir>"
[ -d "$linux_dir" ] ||
	die "Linux 源码目录不存在: $linux_dir"
[ -d "$patch_dir" ] ||
	die "Linux 补丁目录不存在: $patch_dir"
command -v git >/dev/null 2>&1 ||
	die "缺少命令: git"

first_patch=
for patch in "$patch_dir"/*.patch; do
	[ -f "$patch" ] || continue
	first_patch=$patch
	break
done

if [ -z "$first_patch" ]; then
	printf '==> 没有待应用的 Linux 补丁\n'
	exit 0
fi

state_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-linux-patch-state.XXXXXX")
trap 'rm -rf "$state_dir"' EXIT HUP INT TERM

prepare_state_tree()
{
	destination=$1
	mkdir -p "$destination"

	for state_patch in "$patch_dir"/*.patch; do
		[ -f "$state_patch" ] || continue
		sed -n \
			-e 's|^--- a/||p' \
			-e 's|^+++ b/||p' "$state_patch"
	done | while IFS= read -r relative_path; do
		[ "$relative_path" != "/dev/null" ] || continue
		[ -e "$linux_dir/$relative_path" ] || continue
		mkdir -p "$destination/$(dirname "$relative_path")"
		cp "$linux_dir/$relative_path" "$destination/$relative_path"
	done
}

try_forward_series()
{
	target=$1

	for series_patch in "$patch_dir"/*.patch; do
		[ -f "$series_patch" ] || continue
		git -C "$target" apply --check --whitespace=error "$series_patch" ||
			return 1
		git -C "$target" apply --whitespace=error "$series_patch" ||
			return 1
	done
}

try_reverse_series()
{
	target=$1
	reverse_list="$state_dir/reverse.list"

	find "$patch_dir" -maxdepth 1 -type f -name '*.patch' -print |
		LC_ALL=C sort -r >"$reverse_list"
	while IFS= read -r series_patch; do
		git -C "$target" apply --reverse --check "$series_patch" ||
			return 1
		git -C "$target" apply --reverse "$series_patch" ||
			return 1
	done <"$reverse_list"
}

forward_tree="$state_dir/forward"
prepare_state_tree "$forward_tree"
if try_forward_series "$forward_tree" >/dev/null 2>&1; then
	for patch in "$patch_dir"/*.patch; do
		[ -f "$patch" ] || continue
		printf '==> 应用 Linux 补丁: %s\n' "$(basename "$patch")"
		git -C "$linux_dir" apply --check --whitespace=error "$patch"
		git -C "$linux_dir" apply --whitespace=error "$patch"
	done
	exit 0
fi

reverse_tree="$state_dir/reverse"
prepare_state_tree "$reverse_tree"
if try_reverse_series "$reverse_tree" >/dev/null 2>&1; then
	for patch in "$patch_dir"/*.patch; do
		[ -f "$patch" ] || continue
		printf '==> Linux 补丁已应用: %s\n' "$(basename "$patch")"
	done
	exit 0
fi

die "补丁无法应用且未处于整套已应用状态"
