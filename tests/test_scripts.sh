#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-scripts-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

for script in "$repo_root"/scripts/*.sh "$repo_root"/scripts/lib/common.sh; do
	sh -n "$script"
done

raw_dir="$test_dir/linux/drivers/mtd/nand/raw"
empty_patches="$test_dir/empty-patches"
mkdir -p "$raw_dir" "$empty_patches"
printf 'menu "Raw NAND"\nendmenu\n' >"$raw_dir/Kconfig"
printf 'obj-y += nand_base.o\n' >"$raw_dir/Makefile"
printf 'legacy\n' >"$raw_dir/qemu_3dnand_main.c"
printf 'legacy\n' >"$raw_dir/qemu_3dnand_raid.c"

LINUX_DIR="$test_dir/linux" Q3N_LINUX_PATCH_DIR="$empty_patches" \
	sh "$repo_root/scripts/apply-linux-overlay.sh"
first_kconfig=$(cksum "$raw_dir/Kconfig")
first_makefile=$(cksum "$raw_dir/Makefile")

LINUX_DIR="$test_dir/linux" Q3N_LINUX_PATCH_DIR="$empty_patches" \
	sh "$repo_root/scripts/apply-linux-overlay.sh"
[ "$first_kconfig" = "$(cksum "$raw_dir/Kconfig")" ] ||
	fail "Kconfig changed on the second overlay application"
[ "$first_makefile" = "$(cksum "$raw_dir/Makefile")" ] ||
	fail "Makefile changed on the second overlay application"

for file in \
	qemu_3dnand_module.c qemu_3dnand_init.c qemu_3dnand_flash.c \
	ytmc_nand.c qemu_3dnand_controller.c qemu_3dnand_ecc.c \
	qemu_3dnand_addr.c qemu_3dnand_layout.c qemu_3dnand_layout.h \
	qemu_3dnand_page.c qemu_3dnand_page.h qemu_3dnand_page_raid.c \
	qemu_3dnand_page_raid.h qemu_3dnand_hw.c qemu_3dnand_regs.h \
	qemu_3dnand_priv.h Kconfig.qemu_3dnand Makefile.qemu_3dnand; do
	[ -f "$raw_dir/$file" ] || fail "overlay did not install $file"
done

for legacy in qemu_3dnand_main.c qemu_3dnand_map.c qemu_3dnand_raid.c \
	qemu_3dnand_sched.c qemu_3dnand_kunit.c qemu_3dnand.h; do
	[ ! -e "$raw_dir/$legacy" ] || fail "legacy file remains: $legacy"
done

[ "$(grep -Fc 'source "drivers/mtd/nand/raw/Kconfig.qemu_3dnand"' \
	"$raw_dir/Kconfig")" -eq 1 ] || fail "Kconfig include is not idempotent"
[ "$(grep -Fc 'include $(src)/Makefile.qemu_3dnand' \
	"$raw_dir/Makefile")" -eq 1 ] || fail "Makefile include is not idempotent"

makefile="$raw_dir/Makefile.qemu_3dnand"
for object in qemu_3dnand_module.o qemu_3dnand_init.o \
	qemu_3dnand_flash.o ytmc_nand.o qemu_3dnand_controller.o \
	qemu_3dnand_ecc.o qemu_3dnand_addr.o qemu_3dnand_hw.o \
	qemu_3dnand_page.o qemu_3dnand_layout.o; do
	grep -Fq "$object" "$makefile" ||
		fail "production module does not link $object"
done
grep -Fq \
	'qemu_3dnand-$(CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID) +=' \
	"$makefile" ||
	fail "production module does not conditionally link page RAID"
if grep -Eq 'qemu_3dnand_(raid|sched|main)\.o' "$makefile"; then
	fail "production module still links legacy RAID code"
fi

printf 'ok: layered Q3N Linux overlay verified\n'
