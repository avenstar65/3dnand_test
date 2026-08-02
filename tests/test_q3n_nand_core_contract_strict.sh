#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
contract="$repo_root/tests/test_q3n_nand_core_contract.sh"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-contract-strict.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

fake_bin="$tmp_dir/bin"
mkdir -p "$fake_bin"

cat >"$fake_bin/nm" <<'EOF'
#!/usr/bin/env sh
set -eu

case "$*" in
  *qemu_3dnand.ko*)
    if [ "$1" = -u ]; then
      cat <<'SYMS'
                 U nand_scan_with_ids
                 U nand_cleanup
                 U mtd_device_parse_register
                 U mtd_device_unregister
SYMS
      if [ "${FIXTURE_PRIVATE_UNDEFINED:-0}" = 1 ]; then
        printf '                 U %s\n' \
          "${FIXTURE_PRIVATE_SYMBOL:-q3n_unresolved_nonprovider}"
      fi
    elif [ "$1" = --defined-only ]; then
      cat <<'SYMS'
00000000 t q3n_cmdfunc
00000000 t q3n_waitfunc
00000000 t q3n_read_byte
00000000 t q3n_read_buf
00000000 t q3n_write_buf
00000000 t q3n_select_chip
00000000 t q3n_controller_legacy_init
00000000 t q3n_page_read
00000000 t q3n_page_write
00000000 t q3n_page_read_oob
00000000 t q3n_page_write_oob
00000000 t q3n_page_erase_block
SYMS
      case "${FIXTURE_MODE:?}" in
        raid2|raid4|raid8) printf '00000000 r q3n_raid_page_ops\n' ;;
        multiplane)
          cat <<'SYMS'
00000000 t q3n_multiplane_get_ops
00000000 t q3n_multiplane_layout_build
00000000 t q3n_multiplane_oob_free_region
00000000 t q3n_hw_mp_read_page
00000000 t q3n_hw_mp_program_page
00000000 t q3n_hw_mp_erase_group
SYMS
          ;;
        *) : ;;
      esac
    fi
    ;;
  *nand_base.o*)
    cat <<'SYMS'
00000000 T nand_block_isbad
00000000 T nand_block_markbad
SYMS
    ;;
  *nand_bbt.o*)
    cat <<'SYMS'
00000000 T nand_isbad_bbt
00000000 T nand_markbad_bbt
SYMS
    ;;
esac
EOF
chmod +x "$fake_bin/nm"

cat >"$fake_bin/strings" <<'EOF'
#!/usr/bin/env sh
set -eu

case "${FIXTURE_MODE:?}" in
  multiplane)
    cat <<'STRINGS'
mode=multiplane dies=2 planes/group=4
physical-page=16384 logical-page=65536 logical-oob=4096
pages/block=1600 logical-erasesize=104857600 logical-blocks=416
logical-size=43620761600 image-mode=multiplane
STRINGS
    ;;
  raid2|raid4|raid8)
    printf '%s\n' 'logical page=%u physical page=%u oob=%u'
    ;;
esac
EOF
chmod +x "$fake_bin/strings"

make_fixture()
{
	mode=$1
	fixture="$tmp_dir/$mode"
	raw="$fixture/drivers/mtd/nand/raw"
	mkdir -p "$raw"
	: >"$raw/nand_base.o"
	: >"$raw/nand_bbt.o"
	: >"$raw/qemu_3dnand.o"
	: >"$raw/qemu_3dnand_page.o"

	case "$mode" in
		identity)
			cat >"$fixture/.config" <<'EOF'
CONFIG_MTD_NAND_QEMU_3DNAND=m
CONFIG_MTD_NAND_QEMU_3DNAND_IDENTITY=y
# CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID is not set
# CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE is not set
EOF
			;;
		raid2|raid4|raid8)
			ratio=${mode#raid}
			: >"$raw/qemu_3dnand_page_raid.o"
			cat >"$fixture/.config" <<EOF
CONFIG_MTD_NAND_QEMU_3DNAND=m
# CONFIG_MTD_NAND_QEMU_3DNAND_IDENTITY is not set
CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID=y
# CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE is not set
CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=$ratio
EOF
			;;
		multiplane)
			for object in qemu_3dnand_multiplane_layout.o \
				qemu_3dnand_hw_multiplane.o qemu_3dnand_multiplane.o; do
				: >"$raw/$object"
			done
			cat >"$fixture/.config" <<'EOF'
CONFIG_MTD_NAND_QEMU_3DNAND=m
# CONFIG_MTD_NAND_QEMU_3DNAND_IDENTITY is not set
# CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID is not set
CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE=y
EOF
			;;
	esac

	: >"$raw/qemu_3dnand.ko"
	printf '%s\n' "$fixture"
}

run_contract()
{
	fixture=$1
	expected=$2
	shift 2
	if ! FIXTURE_MODE="$expected" PATH="$fake_bin:$PATH" \
		Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$fixture" \
		Q3N_EXPECTED_MODE="$expected" "$@" sh "$contract"; then
		fail "baseline $expected contract failed"
	fi
}

expect_fail()
{
	name=$1
	shift
	if "$@" >"$tmp_dir/$name.out" 2>"$tmp_dir/$name.err"; then
		fail "$name unexpectedly passed"
	fi
}

identity=$(make_fixture identity)
raid4=$(make_fixture raid4)
raid2=$(make_fixture raid2)
raid8=$(make_fixture raid8)
multiplane=$(make_fixture multiplane)

run_contract "$identity" identity env
run_contract "$raid2" raid2 env
run_contract "$raid4" raid4 env
run_contract "$raid8" raid8 env
run_contract "$multiplane" multiplane env

expect_fail private-undefined \
	env FIXTURE_PRIVATE_UNDEFINED=1 FIXTURE_MODE=identity PATH="$fake_bin:$PATH" \
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$identity" \
	Q3N_EXPECTED_MODE=identity sh "$contract"

expect_fail private-undefined-suffixed \
	env FIXTURE_PRIVATE_UNDEFINED=1 \
	FIXTURE_PRIVATE_SYMBOL=q3n_unresolved_nonprovider.isra.0 \
	FIXTURE_MODE=identity PATH="$fake_bin:$PATH" \
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$identity" \
	Q3N_EXPECTED_MODE=identity sh "$contract"

expect_fail missing-expected \
	env FIXTURE_MODE=identity PATH="$fake_bin:$PATH" \
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$identity" \
	sh "$contract"

missing="$tmp_dir/missing-config"
cp -R "$identity" "$missing"
rm "$missing/.config"
expect_fail missing-config \
	env FIXTURE_MODE=identity PATH="$fake_bin:$PATH" \
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$missing" \
	Q3N_EXPECTED_MODE=identity sh "$contract"

expect_fail stale-mode \
	env FIXTURE_MODE=multiplane PATH="$fake_bin:$PATH" \
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$identity" \
	Q3N_EXPECTED_MODE=multiplane sh "$contract"

expect_fail raid-ratio \
	env FIXTURE_MODE=raid8 PATH="$fake_bin:$PATH" \
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$raid4" \
	Q3N_EXPECTED_MODE=raid8 sh "$contract"

expect_fail raid2-ratio \
	env FIXTURE_MODE=raid4 PATH="$fake_bin:$PATH" \
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$raid4" \
	Q3N_EXPECTED_MODE=raid2 sh "$contract"

stale="$tmp_dir/stale-artifact"
cp -R "$identity" "$stale"
sleep 1
touch "$stale/.config"
expect_fail stale-artifact \
	env FIXTURE_MODE=identity PATH="$fake_bin:$PATH" \
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$stale" \
	Q3N_EXPECTED_MODE=identity sh "$contract"

missing_object="$tmp_dir/missing-object"
cp -R "$raid4" "$missing_object"
rm "$missing_object/drivers/mtd/nand/raw/qemu_3dnand_page_raid.o"
expect_fail missing-object \
	env FIXTURE_MODE=raid4 PATH="$fake_bin:$PATH" \
	Q3N_REQUIRE_KERNEL_BUILD=1 Q3N_KERNEL_BUILD_DIR="$missing_object" \
	Q3N_EXPECTED_MODE=raid4 sh "$contract"

printf 'ok: strict compiled Q3N contract rejects private unresolved symbols, stale modes, and stale artifacts\n'
