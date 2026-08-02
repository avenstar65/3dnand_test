#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
driver_makefile="$repo_root/linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-page-raid-config.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

contains_word()
{
	words=$1
	want=$2

	for word in $words; do
		[ "$word" = "$want" ] && return 0
	done
	return 1
}

cat >"$tmp_dir/Makefile" <<EOF
include $driver_makefile

.PHONY: objects
objects:
	@printf '%s\n' "\$(qemu_3dnand-y)"
EOF

check_profile()
{
	raid=$1
	multiplane=$2
	data_pages=$3
	want_raid_object=$4
	want_multiplane_objects=$5
	objects=$(make -s -f "$tmp_dir/Makefile" objects \
		CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID="$raid" \
		CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE="$multiplane" \
		CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES="$data_pages")

	contains_word "$objects" qemu_3dnand_page.o ||
		fail "PAGE_RAID=$raid N=$data_pages omitted qemu_3dnand_page.o"
	contains_word "$objects" qemu_3dnand_layout.o ||
		fail "PAGE_RAID=$raid N=$data_pages omitted qemu_3dnand_layout.o"
	for object in qemu_3dnand_multiplane_layout.o \
		qemu_3dnand_hw_multiplane.o qemu_3dnand_multiplane.o; do
		if [ "$want_multiplane_objects" = yes ]; then
			contains_word "$objects" "$object" ||
				fail "PAGE_RAID=$raid MP=$multiplane N=$data_pages omitted $object"
		elif contains_word "$objects" "$object"; then
			fail "PAGE_RAID=$raid MP=$multiplane N=$data_pages unexpectedly linked $object"
		fi
	done

	if [ "$want_raid_object" = yes ]; then
		contains_word "$objects" qemu_3dnand_page_raid.o ||
			fail "PAGE_RAID=$raid N=$data_pages omitted qemu_3dnand_page_raid.o"
	elif contains_word "$objects" qemu_3dnand_page_raid.o; then
		fail "PAGE_RAID=$raid N=$data_pages unexpectedly linked qemu_3dnand_page_raid.o"
	fi
}

check_profile n n 4 no no
check_profile y n 2 yes no
check_profile y n 4 yes no
check_profile y n 8 yes no
check_profile n y 4 no yes

printf 'ok: identity, Page RAID, and multi-plane object composition verified\n'
