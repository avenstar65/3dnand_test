#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-multiplane-config.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

assert_config_has()
{
	config_line=$1
	grep -Fqx "$config_line" "$out_dir/.config" ||
		fail "generated config is missing: $config_line"
}

assert_config_lacks()
{
	config_line=$1
	if grep -Fqx "$config_line" "$out_dir/.config"; then
		fail "generated config unexpectedly contains: $config_line"
	fi
}

assert_selected_fragment()
{
	wanted_fragment=$1
	for mode_fragment in \
		q3n-identity.fragment q3n-page-raid-2.fragment \
		q3n-page-raid-4.fragment q3n-page-raid-8.fragment \
		q3n-multiplane.fragment; do
		if [ "$mode_fragment" = "$wanted_fragment" ]; then
			grep -Fqx "$repo_root/configs/linux/$mode_fragment" "$merge_log" ||
				fail "merge did not receive $mode_fragment"
		elif grep -Fqx "$repo_root/configs/linux/$mode_fragment" "$merge_log"; then
			fail "merge received unrelated mode fragment: $mode_fragment"
		fi
	done
}

run_configure()
{
	selected_mode=$1
	: >"$merge_log"
	if [ -n "$selected_mode" ]; then
		if ! LC_ALL=C PATH="$fake_bin:$PATH" LINUX_DIR="$linux_dir" BUILD_DIR="$build_root" \
			Q3N_LINUX_PATCH_DIR="$empty_patches" FAKE_MERGE_LOG="$merge_log" \
			sh "$repo_root/scripts/configure-kernel.sh" --q3n-mode "$selected_mode" \
			>"$tmp_dir/configure.out" 2>"$tmp_dir/configure.err"; then
			cat "$tmp_dir/configure.err" >&2
			fail "configure-kernel failed for mode: $selected_mode"
		fi
	else
		if ! LC_ALL=C PATH="$fake_bin:$PATH" LINUX_DIR="$linux_dir" BUILD_DIR="$build_root" \
			Q3N_LINUX_PATCH_DIR="$empty_patches" FAKE_MERGE_LOG="$merge_log" \
			sh "$repo_root/scripts/configure-kernel.sh" \
			>"$tmp_dir/configure.out" 2>"$tmp_dir/configure.err"; then
			cat "$tmp_dir/configure.err" >&2
			fail "configure-kernel failed for default mode"
		fi
	fi

	[ -n "$selected_mode" ] || selected_mode=identity
	grep -Fq "Q3N storage mode: $selected_mode" "$tmp_dir/configure.out" ||
		fail "configure-kernel did not report Q3N storage mode: $selected_mode"
}

linux_dir="$tmp_dir/linux/linux-6.9.0"
build_root="$tmp_dir/build"
out_dir="$build_root/linux-6.9.0"
empty_patches="$tmp_dir/empty-patches"
fake_bin="$tmp_dir/bin"
merge_log="$tmp_dir/merge.args"
mkdir -p "$linux_dir/drivers/mtd/nand/raw" "$linux_dir/scripts/kconfig" \
	"$empty_patches" "$fake_bin"
printf 'menu "Raw NAND"\nendmenu\n' >"$linux_dir/drivers/mtd/nand/raw/Kconfig"
printf 'obj-y += nand_base.o\n' >"$linux_dir/drivers/mtd/nand/raw/Makefile"

cat >"$fake_bin/make" <<'EOF'
#!/usr/bin/env sh
set -eu

out_dir=
defconfig=no
olddefconfig=no
for arg in "$@"; do
	case "$arg" in
		O=*) out_dir=${arg#O=} ;;
		x86_64_defconfig) defconfig=yes ;;
		olddefconfig) olddefconfig=yes ;;
	esac
done
mkdir -p "$out_dir"
if [ "$defconfig" = yes ]; then
	# Keep a previous .config so a mode fragment must explicitly clear old choices.
	[ -f "$out_dir/.config" ] || : >"$out_dir/.config"
elif [ "$olddefconfig" = yes ] && \
	! grep -Fqx 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID=y' "$out_dir/.config"; then
	grep -Fvx 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=2' "$out_dir/.config" | \
		grep -Fvx 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=4' | \
		grep -Fvx 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=8' \
		>"$out_dir/.config.tmp" || true
	mv "$out_dir/.config.tmp" "$out_dir/.config"
fi
EOF
chmod +x "$fake_bin/make"

cat >"$linux_dir/scripts/kconfig/merge_config.sh" <<'EOF'
#!/usr/bin/env sh
set -eu

log=${FAKE_MERGE_LOG:?}
printf '%s\n' "$@" >"$log"
[ "$1" = -m ] && shift
[ "$1" = -O ] && out_dir=$2 && shift 2
config=$1
shift
mkdir -p "$out_dir"
[ -f "$config" ] || : >"$config"

set_symbol()
{
	line=$1
	case "$line" in
		CONFIG_*=*) key=${line%%=*} ;;
		'# CONFIG_'*' is not set') key=${line#\# }; key=${key% is not set} ;;
		*) return 0 ;;
	esac
	tmp="$config.tmp"
	awk -v key="$key" \
		'$0 != "# " key " is not set" && index($0, key "=") != 1' \
		"$config" >"$tmp"
	printf '%s\n' "$line" >>"$tmp"
	mv "$tmp" "$config"
}

for fragment in "$@"; do
	while IFS= read -r line || [ -n "$line" ]; do
		set_symbol "$line"
	done <"$fragment"
done
EOF
chmod +x "$linux_dir/scripts/kconfig/merge_config.sh"

# The production change that must break these assertions is an incorrect mode
# mapping, a missing clear setting, or ignored/invalid --q3n-mode input.
run_configure ''
assert_selected_fragment q3n-identity.fragment
assert_config_has 'CONFIG_MTD_NAND_QEMU_3DNAND_IDENTITY=y'
assert_config_has '# CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID is not set'
assert_config_has '# CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE is not set'
assert_config_lacks 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=2'
assert_config_lacks 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=4'
assert_config_lacks 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=8'

for case_data in \
	'identity:q3n-identity.fragment:CONFIG_MTD_NAND_QEMU_3DNAND_IDENTITY=y' \
	'page-raid-2:q3n-page-raid-2.fragment:CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=2' \
	'page-raid-4:q3n-page-raid-4.fragment:CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=4' \
	'page-raid-8:q3n-page-raid-8.fragment:CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=8' \
	'multiplane:q3n-multiplane.fragment:CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE=y'; do
	mode=${case_data%%:*}
	rest=${case_data#*:}
	fragment=${rest%%:*}
	want=${rest#*:}
	run_configure "$mode"
	assert_selected_fragment "$fragment"
	assert_config_has "$want"
	done

run_configure page-raid-8
assert_config_has 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=8'
run_configure multiplane
assert_config_has 'CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE=y'
assert_config_has '# CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID is not set'
assert_config_lacks 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=8'

if LC_ALL=C PATH="$fake_bin:$PATH" LINUX_DIR="$linux_dir" BUILD_DIR="$build_root" \
	Q3N_LINUX_PATCH_DIR="$empty_patches" FAKE_MERGE_LOG="$merge_log" \
	sh "$repo_root/scripts/configure-kernel.sh" --q3n-mode unknown \
	>"$tmp_dir/unknown.out" 2>"$tmp_dir/unknown.err"; then
	fail 'unknown Q3N mode unexpectedly succeeded'
fi
grep -Fq 'unknown' "$tmp_dir/unknown.err" ||
	fail 'unknown Q3N mode error does not identify the invalid value'
grep -Fq 'page-raid-8' "$tmp_dir/unknown.err" ||
	fail 'unknown Q3N mode error does not list available values'

printf 'ok: Q3N storage mode configuration selection verified\n'
