#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc=${CC:-cc}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-multiplane-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

"$cc" -std=c11 -Wall -Wextra -Werror -DQ3N_HOST_TEST \
	-DCONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE \
	-I"$repo_root/linux/drivers/mtd/nand/raw" \
	"$repo_root/tests/test_q3n_multiplane.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_page.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_layout.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.c" \
	-o "$test_dir/test_q3n_multiplane"

"$test_dir/test_q3n_multiplane"

"$cc" -std=c11 -Wall -Wextra -Werror -DQ3N_HOST_TEST \
	-DCONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE -DQ3N_TEST_ALLOC_FAIL \
	-Dmalloc=q3n_test_malloc \
	-I"$repo_root/linux/drivers/mtd/nand/raw" \
	"$repo_root/tests/test_q3n_multiplane.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_page.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_layout.c" \
	"$repo_root/linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.c" \
	-o "$test_dir/test_q3n_multiplane_alloc_fail"

"$test_dir/test_q3n_multiplane_alloc_fail"
