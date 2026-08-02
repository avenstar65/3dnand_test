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
mkdir -p "$wrapper_repo/scripts/lib" "$wrapper_repo/work"
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
printf '%s\n' 'q3n multi-plane guest complete logical_block=3 die=1 block_in_plane=1 page=0 main_digest=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
printf '%s\n' 'MTD smoke 测试通过，关闭虚拟机'
EOF
cat >"$wrapper_repo/scripts/q3n-multiplane-media-verify.sh" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' "$@" >"$Q3N_TEST_VERIFY_LOG"
[ "${Q3N_TEST_VERIFY_FAIL:-0}" = 0 ] || exit 1
EOF
chmod +x "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" \
	"$wrapper_repo/scripts/run-qemu.sh" \
	"$wrapper_repo/scripts/q3n-multiplane-media-verify.sh"
verify_log="$wrapper_repo/verify.argv"
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
grep -Fqx -- --expected-erased-digest "$verify_log" ||
	fail "multiplane verifier did not receive the NAND Core erase digest"
grep -Fqx 71189f7fb6aed638640078fba3a35fda6c39c8962e74dcc75935aac948da9063 \
	"$verify_log" || fail "multiplane verifier received the wrong erase digest"
if Q3N_TEST_WRAPPER_LOG="$wrapper_log" Q3N_TEST_VERIFY_LOG="$verify_log" \
	Q3N_TEST_VERIFY_FAIL=1 WORK_DIR="$wrapper_repo/work" \
	sh "$wrapper_repo/scripts/q3n-multiplane-smoke.sh" >/dev/null 2>&1; then
	fail "multiplane wrapper accepted a lower-level digest mismatch"
fi

printf 'ok: Q3N Linux/QEMU overlays and isolated storage-mode matrix verified\n'
