#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc=${CC:-cc}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-hw-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

"$cc" -std=c11 -Wall -Wextra -Werror -DQ3N_HOST_TEST \
	-I"$repo_root/linux/drivers/mtd/nand/raw" \
	"$repo_root/tests/test_q3n_hw.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_hw.c" \
	-o "$test_dir/test_q3n_hw"

"$test_dir/test_q3n_hw"
