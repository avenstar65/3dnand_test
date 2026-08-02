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
	qemu_3dnand_module.c qemu_3dnand_init.c qemu_3dnand_init.h \
	qemu_3dnand_flash.c qemu_3dnand_flash.h ytmc_nand.c ytmc_nand.h \
	qemu_3dnand_controller.c qemu_3dnand_controller.h qemu_3dnand_ecc.c \
	qemu_3dnand_ecc.h qemu_3dnand_addr.c qemu_3dnand_addr.h \
	qemu_3dnand_layout.c qemu_3dnand_layout.h qemu_3dnand_page.c \
	qemu_3dnand_page.h qemu_3dnand_page_raid.c qemu_3dnand_page_raid.h \
	qemu_3dnand_hw.c qemu_3dnand_hw.h qemu_3dnand_regs.h qemu_3dnand_priv.h \
	qemu_3dnand_multiplane_layout.c qemu_3dnand_multiplane_layout.h \
	qemu_3dnand_hw_multiplane.c qemu_3dnand_hw_multiplane.h \
	qemu_3dnand_multiplane.c qemu_3dnand_multiplane.h \
	Kconfig.qemu_3dnand Makefile.qemu_3dnand; do
	[ -f "$raw_dir/$file" ] || fail "overlay did not install $file"
	cmp "$repo_root/linux/drivers/mtd/nand/raw/$file" "$raw_dir/$file" ||
		fail "overlay content differs for $file"
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

qemu_dir="$test_dir/qemu"
mkdir -p "$qemu_dir/hw/block" "$qemu_dir/include/hw/mtd"
printf "system_ss.add(when: 'CONFIG_Q3N_NAND', if_true: files('q3n-nand.c'))\n" \
	>"$qemu_dir/hw/block/meson.build"
printf 'menu "Block"\nendmenu\n' >"$qemu_dir/hw/block/Kconfig"

QEMU_DIR="$qemu_dir" sh "$repo_root/scripts/apply-qemu-overlay.sh"
qemu_first_checksum=$(cksum "$qemu_dir/hw/block/meson.build" \
	"$qemu_dir/hw/block/Kconfig")
QEMU_DIR="$qemu_dir" sh "$repo_root/scripts/apply-qemu-overlay.sh"
[ "$qemu_first_checksum" = "$(cksum "$qemu_dir/hw/block/meson.build" \
	"$qemu_dir/hw/block/Kconfig")" ] ||
	fail "QEMU overlay changed on the second application"

for file in q3n-nand.c q3n-pci.c q3n-media.c q3n-multiplane.c \
	q3n-multiplane.h q3n-media-overlay.h; do
	cmp "$repo_root/qemu/hw/mtd/$file" "$qemu_dir/hw/block/$file" ||
		fail "QEMU overlay content differs for $file"
done
for file in q3n-nand.h q3n-media.h; do
	cmp "$repo_root/qemu/include/hw/mtd/$file" "$qemu_dir/include/hw/mtd/$file" ||
		fail "QEMU header overlay content differs for $file"
done
meson_line="system_ss.add(when: 'CONFIG_Q3N_NAND', if_true: files('q3n-media.c', 'q3n-multiplane.c', 'q3n-nand.c', 'q3n-pci.c'))"
[ "$(grep -Fxc "$meson_line" "$qemu_dir/hw/block/meson.build")" -eq 1 ] ||
	fail "QEMU meson output does not have the exact Q3N source composition"

matrix_repo="$test_dir/matrix-repo"
matrix_bin="$matrix_repo/bin"
matrix_log="$matrix_repo/invocations.log"
mkdir -p "$matrix_repo/scripts/lib" "$matrix_repo/tests" "$matrix_repo/linux" \
	"$matrix_bin"
cp "$repo_root/scripts/q3n-kernel-matrix.sh" \
	"$matrix_repo/scripts/q3n-kernel-matrix.sh"
cp "$repo_root/scripts/lib/common.sh" "$matrix_repo/scripts/lib/common.sh"
cat >"$matrix_repo/scripts/configure-kernel.sh" <<'EOF'
#!/usr/bin/env sh
set -eu
mkdir -p "$BUILD_DIR/linux-7.0.12"
printf 'configure|%s|%s|%s\n' "${BUILD_DIR:-}" "$1" "$2" >>"$Q3N_TEST_LOG"
EOF
cat >"$matrix_repo/tests/test_q3n_nand_core_contract.sh" <<'EOF'
#!/usr/bin/env sh
set -eu
[ -f "$Q3N_KERNEL_BUILD_DIR/drivers/mtd/nand/raw/qemu_3dnand.ko" ] ||
	exit 1
[ -f "$Q3N_KERNEL_BUILD_DIR/Module.symvers" ] || exit 1
printf 'contract|%s|%s|%s\n' "${Q3N_EXPECTED_MODE:-}" \
	"${Q3N_KERNEL_BUILD_DIR:-}" "${Q3N_REQUIRE_KERNEL_BUILD:-}" >>"$Q3N_TEST_LOG"
EOF
cat >"$matrix_bin/uname" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' Linux
EOF
cat >"$matrix_bin/make" <<'EOF'
#!/usr/bin/env sh
set -eu
output=
for argument in "$@"; do
	case "$argument" in
		O=*) output=${argument#O=} ;;
	esac
done
printf 'make|%s|%s|%s\n' "${BUILD_DIR:-}" \
	"${KBUILD_EXTRA_SYMBOLS:-}" "$*" >>"$Q3N_TEST_LOG"
case "$*" in
	*vmlinux)
		mkdir -p "$output"
		: >"$output/vmlinux.o"
		;;
	*modules)
		mkdir -p "$output"
		: >"$output/Module.symvers"
		mkdir -p "$output/drivers/mtd/nand/raw"
		: >"$output/drivers/mtd/nand/raw/qemu_3dnand.ko"
		;;
	*drivers/mtd/nand/raw/qemu_3dnand.ko)
		mkdir -p "$output/drivers/mtd/nand/raw"
		: >"$output/drivers/mtd/nand/raw/qemu_3dnand.ko"
		: >"$output/Module.symvers"
		;;
esac
EOF
chmod +x "$matrix_repo/scripts/configure-kernel.sh" \
	"$matrix_repo/tests/test_q3n_nand_core_contract.sh" \
	"$matrix_bin/uname" "$matrix_bin/make"

Q3N_TEST_LOG="$matrix_log" PATH="$matrix_bin:$PATH" \
	WORK_DIR="$matrix_repo/work" LINUX_DIR="$matrix_repo/linux/linux-7.0.12" \
	sh "$matrix_repo/scripts/q3n-kernel-matrix.sh"

[ "$(grep -c '^configure|' "$matrix_log")" -eq 5 ] ||
	fail "matrix did not configure exactly five modes"
[ "$(grep -c '^make|' "$matrix_log")" -eq 6 ] ||
	fail "matrix did not build one ABI seed and five independent module artifacts"
[ "$(grep -c '^contract|' "$matrix_log")" -eq 5 ] ||
	fail "matrix did not validate exactly five module artifacts"

while IFS='|' read -r mode expected; do
	profile="$matrix_repo/work/build-matrix/$mode"
	grep -Fqx "configure|$profile|--q3n-mode|$mode" "$matrix_log" ||
		fail "matrix did not configure $mode in its isolated root"
	grep -Fqx "contract|$expected|$profile/linux-7.0.12|1" "$matrix_log" ||
		fail "matrix did not validate $mode with its explicit contract mode"
	grep -Fq "make|||-C $matrix_repo/linux/linux-7.0.12 O=$profile/linux-7.0.12 ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu- modules" \
		"$matrix_log" ||
		fail "matrix did not build a fresh complete module graph for $mode"
done <<'EOF'
identity|identity
page-raid-2|raid2
page-raid-4|raid4
page-raid-8|raid8
multiplane|multiplane
EOF

[ "$(awk -F'|' '/^configure\|/ { print $2 }' "$matrix_log" | sort -u | wc -l | tr -d ' ')" -eq 5 ] ||
	fail "matrix build roots are not unique"
[ "$(grep -c ' vmlinux$' "$matrix_log")" -eq 1 ] ||
	fail "matrix did not create exactly one clean kernel ABI seed"
grep -Fq "make|||-C $matrix_repo/linux/linux-7.0.12 O=$matrix_repo/work/build-matrix/identity/linux-7.0.12 ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu- vmlinux" \
	"$matrix_log" ||
	fail "matrix did not create the ABI seed from the identity root"
grep -Fq "make|||-C $matrix_repo/linux/linux-7.0.12 O=$matrix_repo/work/build-matrix/identity/linux-7.0.12 ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu- modules" \
	"$matrix_log" ||
	fail "matrix did not create a complete kernel module symbol seed"
[ "$(grep -c ' modules$' "$matrix_log")" -eq 5 ] ||
	fail "matrix did not rebuild Q3N through a complete module target for every mode"

# run-qemu mode selection is exercised through a fake QEMU binary.  These are
# boundary tests: the script must resolve one image, verify the selected
# kernel profile before launch, and leave unrelated media untouched.
runtime_root="$test_dir/runtime"
runtime_bin="$runtime_root/bin"
runtime_work="$runtime_root/work"
runtime_linux="$runtime_root/linux/linux-7.0.12"
runtime_qemu_log="$runtime_root/qemu.argv"
mkdir -p "$runtime_bin" "$runtime_linux" \
	"$runtime_work/build/qemu-11.0.2" \
	"$runtime_work/build/linux-7.0.12/arch/x86/boot" \
	"$runtime_work/rootfs"
runtime_media_dir=$(CDPATH= cd -- "$runtime_work" && pwd -P)/media
: >"$runtime_work/build/linux-7.0.12/arch/x86/boot/bzImage"
: >"$runtime_work/rootfs/initramfs.cpio.gz"
cat >"$runtime_bin/uname" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' Linux
EOF
cat >"$runtime_work/build/qemu-11.0.2/qemu-system-x86_64-unsigned" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' "$@" >>"$Q3N_TEST_QEMU_LOG"
EOF
chmod +x "$runtime_bin/uname" \
	"$runtime_work/build/qemu-11.0.2/qemu-system-x86_64-unsigned"

run_qemu_fixture() {
	PATH="$runtime_bin:$PATH" Q3N_TEST_QEMU_LOG="$runtime_qemu_log" \
	WORK_DIR="$runtime_work" LINUX_DIR="$runtime_linux" \
	sh "$repo_root/scripts/run-qemu.sh" "$@"
}

printf '%s\n' 'CONFIG_MTD_NAND_QEMU_3DNAND_IDENTITY=y' \
	>"$runtime_work/build/linux-7.0.12/.config"
: >"$runtime_qemu_log"
run_qemu_fixture
grep -Fqx "driver=file,filename=$runtime_media_dir/q3n-nand.raw,node-name=q3n-file" \
	"$runtime_qemu_log" || fail "identity mode did not select the legacy image"

printf '%s\n' 'CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID=y' \
	>"$runtime_work/build/linux-7.0.12/.config"
: >"$runtime_qemu_log"
run_qemu_fixture --nand-mode page-raid
grep -Fqx "driver=file,filename=$runtime_media_dir/q3n-nand.raw,node-name=q3n-file" \
	"$runtime_qemu_log" || fail "page-raid mode did not retain the legacy image"

printf '%s\n' 'CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE=y' \
	>"$runtime_work/build/linux-7.0.12/.config"
printf 'legacy-image-must-survive\n' >"$runtime_work/media/q3n-nand.raw"
printf 'stale-multiplane\n' >"$runtime_work/media/q3n-nand-multiplane.raw"
: >"$runtime_qemu_log"
run_qemu_fixture --nand-mode multiplane --fresh-nand
grep -Fqx "driver=file,filename=$runtime_media_dir/q3n-nand-multiplane.raw,node-name=q3n-file" \
	"$runtime_qemu_log" || fail "multiplane mode did not select its dedicated image"
[ "$(cat "$runtime_work/media/q3n-nand.raw")" = 'legacy-image-must-survive' ] ||
	fail "fresh multiplane mode changed the legacy image"
[ ! -s "$runtime_work/media/q3n-nand-multiplane.raw" ] ||
	fail "fresh multiplane mode did not reset only its resolved image"

explicit_image="$runtime_work/media/explicit NAND.raw"
explicit_image_canonical="$runtime_media_dir/explicit NAND.raw"
printf 'explicit-stale\n' >"$explicit_image"
: >"$runtime_qemu_log"
run_qemu_fixture --nand-mode multiplane --nand-image "$explicit_image" --fresh-nand
grep -Fqx "driver=file,filename=$explicit_image_canonical,node-name=q3n-file" \
	"$runtime_qemu_log" || fail "explicit NAND image did not override the path"
[ ! -s "$explicit_image" ] || fail "fresh did not reset the explicit image"
[ "$(cat "$runtime_work/media/q3n-nand.raw")" = 'legacy-image-must-survive' ] ||
	fail "fresh explicit mode changed another image"

printf '%s\n' 'CONFIG_MTD_NAND_QEMU_3DNAND_IDENTITY=y' \
	>"$runtime_work/build/linux-7.0.12/.config"
rm -f "$runtime_qemu_log"
if run_qemu_fixture --nand-mode multiplane; then
	fail "multiplane launch accepted an identity kernel config"
fi
[ ! -e "$runtime_qemu_log" ] ||
	fail "mode/config mismatch launched QEMU"

. "$repo_root/rootfs/profile.d/mtd.sh"
mtd_q3n_multiplane_smoke() { printf 'guest-multiplane-command-ran\n'; }
guest_command=$(mtd_smoke_command_for q3n-multiplane-smoke) ||
	fail "multiplane guest stage did not resolve"
[ "$guest_command" = mtd_q3n_multiplane_smoke ] ||
	fail "multiplane guest stage resolved the wrong command"
[ "$("$guest_command")" = guest-multiplane-command-ran ] ||
	fail "resolved multiplane guest command did not execute"

wrapper_repo="$test_dir/multiplane-wrapper"
wrapper_log="$wrapper_repo/run-qemu.argv"
mkdir -p "$wrapper_repo/scripts/lib" "$wrapper_repo/work/media"
cp "$repo_root/scripts/lib/common.sh" "$wrapper_repo/scripts/lib/common.sh"
[ -f "$repo_root/scripts/q3n-multiplane-smoke.sh" ] ||
	fail "multiplane host wrapper is missing"
cp "$repo_root/scripts/q3n-multiplane-smoke.sh" \
	"$wrapper_repo/scripts/q3n-multiplane-smoke.sh"
[ -f "$repo_root/scripts/q3n-multiplane-media-verify.sh" ] ||
	fail "multiplane media verifier is missing"
cp "$repo_root/scripts/q3n-multiplane-media-verify.sh" \
	"$wrapper_repo/scripts/q3n-multiplane-media-verify.sh"
cat >"$wrapper_repo/scripts/run-qemu.sh" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' "$@" >"$Q3N_TEST_WRAPPER_LOG"
metadata="q3n multi-plane guest complete logical_block=3 die=${Q3N_TEST_GUEST_DIE:-1} block_in_plane=${Q3N_TEST_GUEST_BLOCK:-1} page=${Q3N_TEST_GUEST_PAGE:-0} main_digest=${Q3N_TEST_GUEST_DIGEST:-f790d342cca81bc826050f0b6ce23ce7b4c06c7f174ce97c499653e4202fd450}"
printf '%s\n' "$metadata"
[ "${Q3N_TEST_DUPLICATE_METADATA:-0}" = 0 ] || printf '%s\n' "$metadata"
[ "${Q3N_TEST_MALFORMED_METADATA:-0}" = 0 ] || printf '%s\n' "$metadata injected"
if [ "${Q3N_TEST_NO_SUCCESS_STAGE:-0}" = 0 ]; then
  printf '%s\n' 'MTD smoke 测试通过，关闭虚拟机'
fi
[ "${Q3N_TEST_DUPLICATE_SUCCESS:-0}" = 0 ] || \
  printf '%s\n' 'MTD smoke 测试通过，关闭虚拟机'
if [ "${Q3N_TEST_NO_POWERDOWN:-0}" = 0 ]; then
  printf '%s\n' '[    3.433730] reboot: Power down'
fi
[ "${Q3N_TEST_DUPLICATE_POWERDOWN:-0}" = 0 ] || \
  printf '%s\n' '[    3.433730] reboot: Power down'
[ "${Q3N_TEST_MALFORMED_POWERDOWN:-0}" = 0 ] || \
  printf '%s\n' '[    3.433730] reboot: Power down injected'
EOF
cat >"$wrapper_repo/scripts/q3n-multiplane-media-verify.sh" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' "$@" >"$Q3N_TEST_VERIFY_LOG"
if [ "${Q3N_TEST_VERIFY_MUTATE:-0}" != 0 ]; then
  while [ "$#" -gt 0 ]; do
    if [ "$1" = --image ]; then
      shift
      printf x >>"$1"
      break
    fi
    shift
  done
fi
[ "${Q3N_TEST_VERIFY_FAIL:-0}" = 0 ] || exit 1
EOF
chmod +x "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" \
	"$wrapper_repo/scripts/run-qemu.sh" \
	"$wrapper_repo/scripts/q3n-multiplane-media-verify.sh"
verify_log="$wrapper_repo/verify.argv"
: >"$wrapper_repo/work/media/q3n-nand-multiplane.raw"
Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null
grep -Fqx -- --nand-mode "$wrapper_log" ||
	fail "multiplane wrapper omitted the NAND mode flag"
grep -Fqx multiplane "$wrapper_log" || fail "multiplane wrapper selected wrong mode"
grep -Fqx -- --fresh-nand "$wrapper_log" ||
	fail "multiplane wrapper omitted fresh media reset"
grep -Fqx 'MTD_SMOKE=q3n-multiplane-smoke' "$wrapper_log" ||
	fail "multiplane wrapper omitted the guest smoke stage"
grep -Fqx -- --logical-block "$verify_log" ||
	fail "multiplane verifier did not receive the logical block"
grep -Fqx 3 "$verify_log" || fail "multiplane verifier selected wrong block"
grep -Fqx -- --die "$verify_log" ||
	fail "multiplane verifier did not receive the recorded die"
grep -Fqx 1 "$verify_log" || fail "multiplane verifier received the wrong die"
grep -Fqx -- --block-in-plane "$verify_log" ||
	fail "multiplane verifier did not receive the recorded plane block"
grep -Fqx -- --page "$verify_log" ||
	fail "multiplane verifier did not receive the recorded page"
grep -Fqx -- --expected-erased-digest "$verify_log" ||
	fail "multiplane verifier did not receive the NAND Core erase digest"
grep -Fqx 71189f7fb6aed638640078fba3a35fda6c39c8962e74dcc75935aac948da9063 \
	"$verify_log" || fail "multiplane verifier received the wrong erase digest"
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_VERIFY_FAIL=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted a lower-level digest mismatch"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_DUPLICATE_METADATA=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted duplicate guest metadata"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_DUPLICATE_SUCCESS=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted duplicate guest success stage"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_MALFORMED_METADATA=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted injected guest metadata"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_NO_POWERDOWN=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted missing kernel powerdown"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_DUPLICATE_POWERDOWN=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted duplicate kernel powerdown"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_MALFORMED_POWERDOWN=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted injected kernel powerdown"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_NO_SUCCESS_STAGE=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted missing guest success stage"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_GUEST_DIE=0 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted inconsistent guest topology"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_GUEST_DIGEST=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
	WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted an unexpected pre-mark digest"
fi
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_VERIFY_MUTATE=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted verifier mutation of its base image"
fi

# Exercise the guest acceptance function through its command boundary.  The
# fixture makes bad-block MEMREADs fail, records the requested operations, and
# lets the test prove that an unexpected pre-mark digest is fatal.
guest_fixture="$test_dir/multiplane-guest-fixture.sh"
guest_log="$test_dir/multiplane-guest.argv"
cat >"$guest_fixture" <<'EOF'
#!/usr/bin/env sh
set -eu
. "$1"
mtd_load_q3n() { :; }
mtd_find_q3n() { printf '%s\n' 0; }
cat() {
	case "$1" in
		*/writesize) printf '%s\n' 65536 ;;
		*/oobsize) printf '%s\n' 4096 ;;
		*/oobavail) printf '%s\n' 4092 ;;
		*/erasesize) printf '%s\n' 104857600 ;;
		*/size) printf '%s\n' 43620761600 ;;
		*) command cat "$1" ;;
	esac
}
flash_erase() { :; }
modprobe() { :; }
dd() { :; }
sha256sum() { printf '%s  %s\n' "${Q3N_TEST_GUEST_SHA}" "$1"; }
mtd_badblock() {
	printf '%s\n' "$*" >>"$Q3N_TEST_GUEST_LOG"
	case "$1" in
		oob-read) printf '%s\n' 00 ;;
		set) guest_marked=1 ;;
		get) printf '%s\n' "${guest_marked:-0}" ;;
		page-read|page-pattern-read)
			[ "${guest_marked:-0}" = 1 ] && return 1
			;;
	esac
	return 0
}
mtd_q3n_multiplane_smoke
EOF
chmod +x "$guest_fixture"
if Q3N_TEST_GUEST_LOG="$guest_log" \
	Q3N_TEST_GUEST_SHA=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
	sh "$guest_fixture" "$repo_root/rootfs/profile.d/mtd.sh" >/dev/null 2>&1; then
	fail "multiplane guest accepted an unexpected pre-mark digest"
fi
: >"$guest_log"
Q3N_TEST_GUEST_LOG="$guest_log" \
Q3N_TEST_GUEST_SHA=f790d342cca81bc826050f0b6ce23ce7b4c06c7f174ce97c499653e4202fd450 \
sh "$guest_fixture" "$repo_root/rootfs/profile.d/mtd.sh" >/dev/null ||
	fail "multiplane guest fixture did not complete"
grep -Fqx 'page-pattern-write place /dev/mtd0 104792064 0x32 0xa2' "$guest_log" ||
	fail "multiplane guest did not request full PLACE OOB pattern I/O"
grep -Fqx 'page-pattern-read raw /dev/mtd0 104857600 0x33 0xa3' "$guest_log" ||
	fail "multiplane guest did not request full RAW OOB pattern I/O"
grep -Fqx 'page-pattern-read place /dev/mtd0 314572800 0x35 0xa5' "$guest_log" ||
	fail "multiplane guest did not reject normal MEMREAD after markbad"
grep -Fqx 'page-pattern-read raw /dev/mtd0 314572800 0x35 0xa5' "$guest_log" ||
	fail "multiplane guest did not reject raw MEMREAD after markbad"

printf 'ok: Q3N Linux/QEMU overlays and isolated storage-mode matrix verified\n'

# Task 11: exercise the two-boot persistence wrapper at its QEMU boundary.
persist_repo="$test_dir/multiplane-persistence-wrapper"
persist_log="$persist_repo/run-qemu.log"
persist_old="$persist_repo/work/media/q3n-nand.raw"
mkdir -p "$persist_repo/scripts/lib" "$persist_repo/work/media"
cp "$repo_root/scripts/lib/common.sh" "$persist_repo/scripts/lib/common.sh"
[ -f "$repo_root/scripts/q3n-multiplane-persistence-smoke.sh" ] ||
	fail "multi-plane persistence host wrapper is missing"
cp "$repo_root/scripts/q3n-multiplane-persistence-smoke.sh" \
	"$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh"
cat >"$persist_repo/scripts/run-qemu.sh" <<'EOF'
#!/usr/bin/env sh
set -eu
stage= fresh=0 mode= image=
while [ "$#" -gt 0 ]; do
	case "$1" in
		--nand-mode) shift; mode=$1 ;;
		--nand-image) shift; image=$1 ;;
		--fresh-nand) fresh=1 ;;
		--append) shift; stage=${1#MTD_SMOKE=} ;;
	esac
	shift
done
printf 'stage=%s fresh=%s mode=%s image=%s\n' "$stage" "$fresh" "$mode" "$image" >>"$Q3N_TEST_PERSIST_LOG"
image_path=$image
case "$image_path" in
	/*) ;;
	*) image_path="$(dirname -- "$0")/../$image_path" ;;
esac
case "$stage" in
	q3n-multiplane-persist-prepare)
		[ "$fresh" = 1 ] || exit 91
		mkdir -p "$(dirname "$image_path")"
		printf Q3NMEDIA >"$image_path"
		dd if=/dev/zero of="$image_path" bs=1 count=1 seek=67108863 2>/dev/null
		kind=expected
		;;
	q3n-multiplane-persist-verify)
		[ "$fresh" = 0 ] || exit 92
		[ -f "$image_path" ] || exit 93
		kind=verified
		;;
	*) exit 94 ;;
esac
main=${Q3N_TEST_PERSIST_MAIN:-944044fe482bc4e91085c15c5a923a1b9e02eac98d3bce04997d6dbecd2a5b8d}
oob=${Q3N_TEST_PERSIST_OOB:-f0f9ce8608610d597e3416195182a2d1f47d53cf00f1e72e3824a5bc3bfa7ce8}
bbm=${Q3N_TEST_PERSIST_BBM:-00000000}
[ "$kind" = verified ] || main=${Q3N_TEST_PERSIST_PREPARE_MAIN:-$main}
[ "$kind" = verified ] || oob=${Q3N_TEST_PERSIST_PREPARE_OOB:-$oob}
[ "$kind" = verified ] || bbm=${Q3N_TEST_PERSIST_PREPARE_BBM:-$bbm}
[ "$kind" = expected ] || main=${Q3N_TEST_PERSIST_VERIFY_MAIN:-$main}
[ "$kind" = expected ] || oob=${Q3N_TEST_PERSIST_VERIFY_OOB:-$oob}
[ "$kind" = expected ] || bbm=${Q3N_TEST_PERSIST_VERIFY_BBM:-$bbm}
printf 'q3n multi-plane persistence %s main_digest=%s oob_digest=%s bbm=%s\n' "$kind" "$main" "$oob" "$bbm"
[ "${Q3N_TEST_PERSIST_DUPLICATE:-}" != "$kind" ] ||
	printf 'q3n multi-plane persistence %s main_digest=%s oob_digest=%s bbm=%s\n' "$kind" "$main" "$oob" "$bbm"
[ "${Q3N_TEST_PERSIST_MALFORMED:-}" != "$kind" ] ||
	printf 'q3n multi-plane persistence %s main_digest=%s oob_digest=%s bbm=%s injected\n' "$kind" "$main" "$oob" "$bbm"
if [ "$kind" = expected ]; then
	stage_name=prepare
else
	stage_name=verify
fi
printf 'q3n multi-plane persistence %s passed\n' "$stage_name"
[ "${Q3N_TEST_PERSIST_CROSS_RECORD:-}" != "$kind" ] || {
	if [ "$kind" = expected ]; then other_kind=verified; else other_kind=expected; fi
	printf 'q3n multi-plane persistence %s main_digest=%s oob_digest=%s bbm=%s\n' "$other_kind" "$main" "$oob" "$bbm"
}
[ "${Q3N_TEST_PERSIST_CROSS_STAGE:-}" != "$kind" ] || {
	if [ "$kind" = expected ]; then other_stage=verify; else other_stage=prepare; fi
	printf 'q3n multi-plane persistence %s passed\n' "$other_stage"
}
if [ "${Q3N_TEST_PERSIST_CROSS_NAMESPACE:-}" = "$kind" ]; then
	if [ "$kind" = expected ]; then other_stage=verify; else other_stage=prepare; fi
	case "${Q3N_TEST_PERSIST_CROSS_NAMESPACE_VARIANT:-failed}" in
		failed) printf 'q3n multi-plane persistence %s failed\n' "$other_stage" ;;
		injected) printf 'q3n multi-plane persistence %s injected\n' "$other_stage" ;;
		prefixed) printf 'injected q3n multi-plane persistence %s failed\n' "$other_stage" ;;
		suffixed) printf 'q3n multi-plane persistence %s passed injected\n' "$other_stage" ;;
		*) exit 95 ;;
	esac
fi
[ "${Q3N_TEST_PERSIST_PREFIXED:-}" != "$kind" ] ||
	printf 'injected q3n multi-plane persistence %s main_digest=%s oob_digest=%s bbm=%s\n' "$kind" "$main" "$oob" "$bbm"
if [ "${Q3N_TEST_PERSIST_LEGACY_STAGE:-}" = "$kind" ]; then
	case "${Q3N_TEST_PERSIST_LEGACY_ACTION:-}" in
		mutate) printf x >>"$Q3N_TEST_PERSIST_OLD" ;;
		remove) rm -f -- "$Q3N_TEST_PERSIST_OLD" ;;
		replace)
			mv "$Q3N_TEST_PERSIST_OLD" "$Q3N_TEST_PERSIST_OLD.replaced"
			printf replacement >"$Q3N_TEST_PERSIST_OLD"
			;;
	esac
fi
if [ "${Q3N_TEST_PERSIST_LEGACY_RESTORE_STAGE:-}" = "$kind" ]; then
	case "${Q3N_TEST_PERSIST_LEGACY_RESTORE_ACTION:-}" in
		mutate-restore)
			if [ "$kind" = expected ]; then
				cp "$Q3N_TEST_PERSIST_OLD" "$Q3N_TEST_PERSIST_OLD.saved"
				printf x >>"$Q3N_TEST_PERSIST_OLD"
			else
				mv "$Q3N_TEST_PERSIST_OLD.saved" "$Q3N_TEST_PERSIST_OLD"
			fi
			;;
		replace-restore)
			if [ "$kind" = expected ]; then
				mv "$Q3N_TEST_PERSIST_OLD" "$Q3N_TEST_PERSIST_OLD.saved"
				printf replacement >"$Q3N_TEST_PERSIST_OLD"
			else
				mv "$Q3N_TEST_PERSIST_OLD.saved" "$Q3N_TEST_PERSIST_OLD"
			fi
			;;
		*) exit 96 ;;
	esac
fi
printf '%s\n' 'MTD smoke 测试通过，关闭虚拟机'
[ "${Q3N_TEST_PERSIST_NO_POWERDOWN:-0}" = 0 ] &&
	printf '%s\n' '[    3.433730] reboot: Power down'
EOF
chmod +x "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" \
	"$persist_repo/scripts/run-qemu.sh"
if Q3N_TEST_PERSIST_LOG="$persist_log" WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted a missing legacy image"
fi
printf 'legacy-image-must-survive\n' >"$persist_old"
persist_old_before=$(stat -f '%i:%z:%m:%b' "$persist_old" 2>/dev/null ||
	stat -c '%i:%s:%Y:%b' "$persist_old")
Q3N_TEST_PERSIST_LOG="$persist_log" WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null
persist_image_arg="$persist_repo/work/media/q3n-nand-multiplane.raw"
if [ "$(uname -s)" = Darwin ]; then
	persist_image_arg=work/media/q3n-nand-multiplane.raw
fi
grep -Fqx "stage=q3n-multiplane-persist-prepare fresh=1 mode=multiplane image=$persist_image_arg" "$persist_log" ||
	fail "persistence prepare did not use fresh dedicated multi-plane media"
grep -Fqx "stage=q3n-multiplane-persist-verify fresh=0 mode=multiplane image=$persist_image_arg" "$persist_log" ||
	fail "persistence verify did not reuse dedicated multi-plane media"
persist_old_after=$(stat -f '%i:%z:%m:%b' "$persist_old" 2>/dev/null ||
	stat -c '%i:%s:%Y:%b' "$persist_old")
[ "$persist_old_before" = "$persist_old_after" ] ||
	fail "persistence wrapper changed the legacy image"
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_CROSS_RECORD=expected \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted a verified record during prepare"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_CROSS_RECORD=verified \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted an expected record during verify"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_CROSS_STAGE=expected \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted a verify success stage during prepare"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_CROSS_STAGE=verified \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted a prepare success stage during verify"
fi
for persist_phase in expected verified; do
	for persist_variant in failed injected prefixed suffixed; do
		if Q3N_TEST_PERSIST_LOG="$persist_log" \
			Q3N_TEST_PERSIST_CROSS_NAMESPACE="$persist_phase" \
			Q3N_TEST_PERSIST_CROSS_NAMESPACE_VARIANT="$persist_variant" \
			WORK_DIR="$persist_repo/work" \
			sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
			fail "persistence wrapper accepted opposite stage namespace $persist_variant during $persist_phase"
		fi
	done
done
for persist_phase in expected verified; do
	if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_DUPLICATE="$persist_phase" \
		WORK_DIR="$persist_repo/work" \
		sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
		fail "persistence wrapper accepted duplicate $persist_phase metadata"
	fi
	if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_MALFORMED="$persist_phase" \
		WORK_DIR="$persist_repo/work" \
		sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
		fail "persistence wrapper accepted malformed $persist_phase metadata"
	fi
	if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_PREFIXED="$persist_phase" \
		WORK_DIR="$persist_repo/work" \
		sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
		fail "persistence wrapper accepted prefixed $persist_phase metadata"
	fi
done
for persist_action in mutate remove replace; do
	for persist_phase in expected verified; do
		printf 'legacy-image-must-survive\n' >"$persist_old"
		if Q3N_TEST_PERSIST_LOG="$persist_log" \
			Q3N_TEST_PERSIST_OLD="$persist_old" \
			Q3N_TEST_PERSIST_LEGACY_ACTION="$persist_action" \
			Q3N_TEST_PERSIST_LEGACY_STAGE="$persist_phase" \
			WORK_DIR="$persist_repo/work" \
			sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
			fail "persistence wrapper accepted legacy image $persist_action during $persist_phase"
		fi
	done
done
for persist_action in mutate-restore replace-restore; do
	: >"$persist_log"
	printf 'legacy-image-must-survive\n' >"$persist_old"
	if Q3N_TEST_PERSIST_LOG="$persist_log" \
		Q3N_TEST_PERSIST_OLD="$persist_old" \
		Q3N_TEST_PERSIST_LEGACY_RESTORE_ACTION="$persist_action" \
		Q3N_TEST_PERSIST_LEGACY_RESTORE_STAGE=expected \
		WORK_DIR="$persist_repo/work" \
		sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
		fail "persistence wrapper accepted legacy $persist_action across boot boundary"
	fi
[ "$(grep -Ec '^stage=q3n-multiplane-persist-prepare ' "$persist_log")" = 1 ] ||
		fail "legacy $persist_action did not reach prepare boundary"
[ "$(grep -Ec '^stage=q3n-multiplane-persist-verify ' "$persist_log")" = 0 ] ||
		fail "legacy $persist_action was not rejected before verify"
done
: >"$persist_log"
printf 'legacy-image-must-survive\n' >"$persist_old"
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_OLD="$persist_old" \
	Q3N_TEST_PERSIST_LEGACY_ACTION=mutate \
	Q3N_TEST_PERSIST_LEGACY_STAGE=verified WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted legacy mutation during verify"
fi
[ "$(grep -Ec '^stage=q3n-multiplane-persist-prepare ' "$persist_log")" = 1 ] &&
	[ "$(grep -Ec '^stage=q3n-multiplane-persist-verify ' "$persist_log")" = 1 ] ||
	fail "legacy verify mutation did not fail at the verify boundary"
printf 'legacy-symlink-target\n' >"$persist_old.target"
rm -f -- "$persist_old"
ln -s "$(basename -- "$persist_old.target")" "$persist_old"
if Q3N_TEST_PERSIST_LOG="$persist_log" WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted a symlinked legacy image"
fi
rm -f -- "$persist_old"
printf 'legacy-image-must-survive\n' >"$persist_old"
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_MAIN=bad \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted mismatched main metadata"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" \
	Q3N_TEST_PERSIST_VERIFY_MAIN=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted unequal well-formed main digests"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_OOB=bad \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted mismatched OOB metadata"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" \
	Q3N_TEST_PERSIST_VERIFY_OOB=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted unequal well-formed OOB digests"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_BBM=ffffffff \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted mismatched BBM metadata"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_VERIFY_BBM=ffffffff \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted unequal well-formed BBM values"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_DUPLICATE=expected \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted duplicate expected metadata"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_MALFORMED=verified \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted malformed verified metadata"
fi
if Q3N_TEST_PERSIST_LOG="$persist_log" Q3N_TEST_PERSIST_NO_POWERDOWN=1 \
	WORK_DIR="$persist_repo/work" \
	sh "$persist_repo/scripts/q3n-multiplane-persistence-smoke.sh" >/dev/null 2>&1; then
	fail "persistence wrapper accepted missing powerdown record"
fi

persist_prepare_command=$(mtd_smoke_command_for q3n-multiplane-persist-prepare) ||
	fail "multi-plane persistence prepare guest stage did not resolve"
persist_verify_command=$(mtd_smoke_command_for q3n-multiplane-persist-verify) ||
	fail "multi-plane persistence verify guest stage did not resolve"
[ "$persist_prepare_command" = mtd_q3n_multiplane_persist_prepare ] ||
	fail "multi-plane persistence prepare stage resolved the wrong command"
[ "$persist_verify_command" = mtd_q3n_multiplane_persist_verify ] ||
	fail "multi-plane persistence verify stage resolved the wrong command"
if mtd_smoke_command_for q3n-multiplane-persist-restart >/dev/null 2>&1; then
	fail "multi-plane persistence resolver accepted an unknown stage"
fi

# Exercise both guest stages through their command boundary.  The fake helper
# makes a marked block reject PLACE and RAW reads and records full-width OOB
# operations; no production source is inspected.
persist_guest_fixture="$test_dir/multiplane-persistence-guest-fixture.sh"
persist_guest_log="$test_dir/multiplane-persistence-guest.argv"
cat >"$persist_guest_fixture" <<'EOF'
#!/usr/bin/env sh
set -eu
. "$1"
mtd_load_q3n() { :; }
mtd_find_q3n() { printf '%s\n' 0; }
cat() {
	case "$1" in
		*/writesize) printf '%s\n' 65536 ;;
		*/oobsize) printf '%s\n' 4096 ;;
		*/oobavail) printf '%s\n' 4092 ;;
		*/erasesize) printf '%s\n' 104857600 ;;
		*/size) printf '%s\n' 43620761600 ;;
		*) command cat "$1" ;;
	esac
}
flash_erase() { printf 'erase %s\n' "$*" >>"$Q3N_TEST_PERSIST_GUEST_LOG"; }
modprobe() { printf 'modprobe %s\n' "$*" >>"$Q3N_TEST_PERSIST_GUEST_LOG"; }
sync() { printf 'sync\n' >>"$Q3N_TEST_PERSIST_GUEST_LOG"; }
dd() { :; }
sha256sum() {
	case "$1" in
		*main*) printf '%s  %s\n' 944044fe482bc4e91085c15c5a923a1b9e02eac98d3bce04997d6dbecd2a5b8d "$1" ;;
		*oob*) printf '%s  %s\n' f0f9ce8608610d597e3416195182a2d1f47d53cf00f1e72e3824a5bc3bfa7ce8 "$1" ;;
		esac
}
mtd_badblock() {
	printf '%s\n' "$*" >>"$Q3N_TEST_PERSIST_GUEST_LOG"
	case "$1" in
		set) guest_bad=1 ;;
		get) printf '%s\n' "${guest_bad:-0}" ;;
		oob-read) printf '%s\n' 00 ;;
		oob-dump) printf x ;;
		page-pattern-read)
			[ "${guest_bad:-0}" = 1 ] && [ "$4" = 104857600 ] && return 1
			;;
	esac
	return 0
}
mtd_q3n_multiplane_persist_prepare
mtd_q3n_multiplane_persist_verify
EOF
chmod +x "$persist_guest_fixture"
: >"$persist_guest_log"
Q3N_TEST_PERSIST_GUEST_LOG="$persist_guest_log" \
	sh "$persist_guest_fixture" "$repo_root/rootfs/profile.d/mtd.sh" >/dev/null ||
	fail "multi-plane persistence guest stages did not complete"
grep -Fqx 'page-pattern-write raw /dev/mtd0 0 0x5a 0xb1' "$persist_guest_log" ||
	fail "persistence prepare did not program block 0 full raw main/OOB pattern"
grep -Fqx 'page-pattern-read raw /dev/mtd0 0 0x5a 0xb1' "$persist_guest_log" ||
	fail "persistence verify did not validate block 0 full raw main/OOB pattern"
grep -Fqx 'oob-dump raw /dev/mtd0 0 0 4096' "$persist_guest_log" ||
	fail "persistence stages did not digest all 4096 raw OOB bytes"
grep -Fqx 'page-pattern-read place /dev/mtd0 104857600 0x69 0xb9' "$persist_guest_log" ||
	fail "persistence stages did not reject normal reads from the marked block"
grep -Fqx 'page-pattern-read raw /dev/mtd0 104857600 0x69 0xb9' "$persist_guest_log" ||
	fail "persistence stages did not reject raw reads from the marked block"
grep -Fqx sync "$persist_guest_log" ||
	fail "persistence prepare did not synchronize media before powerdown"
