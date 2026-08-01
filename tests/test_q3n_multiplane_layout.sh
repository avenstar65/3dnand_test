#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc=${CC:-cc}
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-multiplane-layout-test.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

"$cc" -std=c11 -Wall -Wextra -Werror -DQ3N_HOST_TEST \
	-I"$repo_root/linux/drivers/mtd/nand/raw" \
	"$repo_root/tests/test_q3n_multiplane_layout.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.c" \
	-o "$tmp_dir/test_q3n_multiplane_layout"

"$tmp_dir/test_q3n_multiplane_layout"
