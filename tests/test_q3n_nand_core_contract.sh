#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${BUILD_DIR:-"$repo_root/work/build"}
kernel_build=${Q3N_KERNEL_BUILD_DIR:-}

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

if [ -z "$kernel_build" ]; then
	kernel_build=$(find "$build_root" -maxdepth 1 -type d \
		-name 'linux-*' 2>/dev/null | sort -V | tail -n 1)
fi

if [ -z "$kernel_build" ] ||
   [ ! -f "$kernel_build/drivers/mtd/nand/raw/qemu_3dnand.ko" ]; then
	if [ "${Q3N_REQUIRE_KERNEL_BUILD:-0}" = 1 ]; then
		fail "compiled Q3N kernel module is unavailable"
	fi
	printf 'ok: NAND Core compiled contract skipped (no kernel build)\n'
	exit 0
fi

module="$kernel_build/drivers/mtd/nand/raw/qemu_3dnand.ko"
nand_base="$kernel_build/drivers/mtd/nand/raw/nand_base.o"
nand_bbt="$kernel_build/drivers/mtd/nand/raw/nand_bbt.o"

command -v nm >/dev/null 2>&1 || fail "nm is unavailable"
[ -f "$nand_base" ] || fail "compiled nand_base.o is unavailable"
[ -f "$nand_bbt" ] || fail "compiled nand_bbt.o is unavailable"

undefined=$(nm -u "$module")
defined=$(nm --defined-only "$module")
base_symbols=$(nm "$nand_base")
bbt_symbols=$(nm "$nand_bbt")

for symbol in q3n_cmdfunc q3n_waitfunc q3n_read_byte q3n_read_buf \
	q3n_write_buf q3n_select_chip q3n_controller_legacy_init; do
	printf '%s\n' "$defined" |
		grep -Eq "[[:space:]][tT][[:space:]]+$symbol$" ||
		fail "Q3N module does not define legacy callback $symbol"
done

if printf '%s\n' "$defined" |
   grep -Eq '[[:space:]][tT][[:space:]]+q3n_.*exec_op$'; then
	fail "Q3N module still defines an exec_op implementation"
fi

for symbol in nand_scan_with_ids nand_cleanup mtd_device_parse_register \
	mtd_device_unregister; do
	printf '%s\n' "$undefined" | grep -Eq "[[:space:]]U[[:space:]]+$symbol$" ||
		fail "Q3N module does not import $symbol"
done

for symbol in nand_block_isbad nand_block_markbad; do
	printf '%s\n' "$base_symbols" |
		grep -Eq "[[:space:]][tT][[:space:]]+$symbol$" ||
		fail "NAND Core does not own $symbol"
done

for symbol in nand_isbad_bbt nand_markbad_bbt; do
	printf '%s\n' "$bbt_symbols" |
		grep -Eq "[[:space:]][tT][[:space:]]+$symbol$" ||
		fail "NAND BBT does not provide $symbol"
done

if printf '%s\n' "$defined" |
   grep -Eiq '[[:space:]]q3n_.*(bbt|badblock|block_isbad|block_markbad)$'; then
	fail "Q3N module defines a private bad-block or BBT implementation"
fi

for direct_mtd in add_mtd_device add_mtd_partitions mtd_device_register; do
	if printf '%s\n' "$undefined" |
	   grep -Eq "[[:space:]]U[[:space:]]+$direct_mtd$"; then
		fail "Q3N module bypasses parse registration through $direct_mtd"
	fi
done

printf 'ok: compiled Q3N module uses legacy callbacks and delegates bad-block/BBT to NAND Core\n'
