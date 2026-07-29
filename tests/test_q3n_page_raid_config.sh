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
	data_pages=$2
	want_raid_object=$3
	objects=$(make -s -f "$tmp_dir/Makefile" objects \
		CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID="$raid" \
		CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES="$data_pages")

	contains_word "$objects" qemu_3dnand_page.o ||
		fail "PAGE_RAID=$raid N=$data_pages omitted qemu_3dnand_page.o"
	contains_word "$objects" qemu_3dnand_layout.o ||
		fail "PAGE_RAID=$raid N=$data_pages omitted qemu_3dnand_layout.o"

	if [ "$want_raid_object" = yes ]; then
		contains_word "$objects" qemu_3dnand_page_raid.o ||
			fail "PAGE_RAID=$raid N=$data_pages omitted qemu_3dnand_page_raid.o"
	elif contains_word "$objects" qemu_3dnand_page_raid.o; then
		fail "PAGE_RAID=$raid N=$data_pages unexpectedly linked qemu_3dnand_page_raid.o"
	fi
}

check_profile n 4 no
check_profile y 2 yes
check_profile y 4 yes
check_profile y 8 yes

printf 'ok: disabled, 2:1, 4:1 and 8:1 Page RAID object composition verified\n'
