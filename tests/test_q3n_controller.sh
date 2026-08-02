#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc=${CC:-cc}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-controller-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

sed -n '/Q3N_CONTROLLER_HELPERS_BEGIN/,/Q3N_CONTROLLER_HELPERS_END/p' \
	"$repo_root/qemu/hw/mtd/q3n-nand.c" \
	>"$test_dir/q3n-controller-helpers.inc"
sed -n '/Q3N_MEDIA_LAYOUT_HELPERS_BEGIN/,/Q3N_MEDIA_LAYOUT_HELPERS_END/p' \
	"$repo_root/qemu/hw/mtd/q3n-media.c" \
	>"$test_dir/q3n-media-layout-helpers.inc"

"$cc" -std=c11 -Wall -Wextra -Werror \
	-I"$repo_root/qemu/hw/mtd" -I"$test_dir" \
	"$repo_root/tests/test_q3n_controller.c" \
	-o "$test_dir/test_q3n_controller"

"$test_dir/test_q3n_controller"
