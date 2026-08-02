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
	if [ "${Q3N_REQUIRE_KERNEL_BUILD:-0}" = 1 ]; then
		fail "Q3N_REQUIRE_KERNEL_BUILD requires Q3N_KERNEL_BUILD_DIR"
	fi
	printf 'ok: NAND Core compiled contract skipped (no explicit kernel build)\n'
	exit 0
fi

if [ ! -f "$kernel_build/drivers/mtd/nand/raw/qemu_3dnand.ko" ]; then
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
command -v strings >/dev/null 2>&1 || fail "strings is unavailable"
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

for symbol in q3n_page_read q3n_page_write q3n_page_read_oob \
	q3n_page_write_oob q3n_page_erase_block; do
	printf '%s\n' "$defined" |
		grep -Eq "[[:space:]][tT][[:space:]]+$symbol$" ||
		fail "Q3N module does not define logical-page entry point $symbol"
done

if grep -q '^CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE=y$' \
   "$kernel_build/.config"; then
	for symbol in q3n_multiplane_get_ops q3n_multiplane_layout_build \
		q3n_multiplane_oob_free_region q3n_hw_mp_read_page \
		q3n_hw_mp_program_page q3n_hw_mp_erase_group; do
		printf '%s\n' "$defined" |
			grep -Eq "[[:space:]][tT][[:space:]]+$symbol$" ||
			fail "multi-plane Q3N module omits $symbol"
	done
	if printf '%s\n' "$defined" |
	   grep -Eq '[[:space:]][dDrR][[:space:]]+q3n_raid_page_ops$'; then
		fail "multi-plane Q3N module includes q3n_raid_page_ops"
	fi
	for literal in \
		'mode=multiplane dies=2 planes/group=4' \
		'physical-page=16384 logical-page=65536 logical-oob=4096' \
		'pages/block=1600 logical-erasesize=104857600 logical-blocks=416' \
		'logical-size=43620761600 image-mode=multiplane'; do
		strings "$module" | grep -Fq "$literal" ||
			fail "multi-plane Q3N module omits init log: $literal"
	done
elif grep -q '^CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID=y$' \
   "$kernel_build/.config"; then
	printf '%s\n' "$defined" |
		grep -Eq '[[:space:]][dDrR][[:space:]]+q3n_raid_page_ops$' ||
		fail "RAID-enabled Q3N module omits q3n_raid_page_ops"
	strings "$module" |
		grep -Fq 'logical page=%u physical page=%u oob=%u' ||
		fail "RAID-enabled Q3N module log omits physical page size"
elif printf '%s\n' "$defined" |
     grep -Eq '[[:space:]][dDrR][[:space:]]+q3n_raid_page_ops$'; then
	fail "RAID-disabled Q3N module includes q3n_raid_page_ops"
fi

if printf '%s\n' "$undefined" |
   grep -Eq '[[:space:]]U[[:space:]]+q3n_multiplane_oob_free_region$'; then
	fail "Q3N module imports an unresolved MP-only OOB layout provider"
fi

if ! grep -q '^CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE=y$' \
     "$kernel_build/.config"; then
	if printf '%s\n' "$undefined" |
	   grep -Eq '[[:space:]]U[[:space:]]+q3n_multiplane_oob_free_region$'; then
		fail "non-multi-plane Q3N module imports MP-only OOB layout provider"
	fi
	if printf '%s\n' "$defined" |
	   grep -Eq '[[:space:]][tT][[:space:]]+q3n_multiplane_oob_free_region$'; then
		fail "non-multi-plane Q3N module defines MP-only OOB layout provider"
	fi
fi

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
