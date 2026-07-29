#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc=${CC:-cc}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-page-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

common_flags="-std=c11 -Wall -Wextra -Werror -DQ3N_HOST_TEST"
include_flags="-I$repo_root/linux/drivers/mtd/nand/raw"

"$cc" $common_flags $include_flags \
	"$repo_root/tests/test_q3n_page.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_page.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_layout.c" \
	-o "$test_dir/test_q3n_page_identity"

"$test_dir/test_q3n_page_identity"

"$cc" $common_flags $include_flags \
	-DCONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID -DQ3N_TEST_RAID \
	"$repo_root/tests/test_q3n_page.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_page.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_layout.c" \
	-o "$test_dir/test_q3n_page_raid"

"$test_dir/test_q3n_page_raid"
