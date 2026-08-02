#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

mkdirs
prepare_log="$work_dir/q3n-multiplane-persist-prepare.log"
verify_log="$work_dir/q3n-multiplane-persist-verify.log"
nand_image="$work_dir/media/q3n-nand-multiplane.raw"
expected_main_digest=944044fe482bc4e91085c15c5a923a1b9e02eac98d3bce04997d6dbecd2a5b8d
expected_oob_digest=f0f9ce8608610d597e3416195182a2d1f47d53cf00f1e72e3824a5bc3bfa7ce8
expected_bbm=00000000

die()
{
	printf '错误: %s\n' "$*" >&2
	exit 1
}

# run-qemu re-enters the Docker build container on macOS.  Give that inner
# invocation a workspace-relative path while retaining the host path for the
# media checks below.
mkdir -p "$(dirname -- "$nand_image")"
image_dir=$(CDPATH= cd -- "$(dirname -- "$nand_image")" && pwd -P)
nand_image="$image_dir/$(basename -- "$nand_image")"
qemu_nand_image=$nand_image
if [ "$(uname -s)" = Darwin ]; then
	workspace_root=$(CDPATH= cd -- "$repo_root" && pwd -P)
	case "$nand_image" in
		"$workspace_root"/*) qemu_nand_image=${nand_image#"$workspace_root"/} ;;
		*) die "multi-plane NAND image is outside the workspace: $nand_image" ;;
	esac
fi

line_count()
{
	sed 's/\r$//' "$1" | grep -E -c "$2" || true
}

image_fingerprint()
{
	stat -f '%i:%z:%m:%b' "$1" 2>/dev/null ||
		stat -c '%i:%s:%Y:%b' "$1"
}

require_one()
{
	log=$1 exact=$2 prefix=$3 description=$4
	[ "$(line_count "$log" "$exact")" = 1 ] &&
		[ "$(line_count "$log" "$prefix")" = 1 ] ||
		die "$description must appear exactly once"
	[ "$(line_count "$log" "${prefix#^}")" = 1 ] ||
		die "$description must not have prefixed or suffixed variants"
}

require_none()
{
	log=$1 pattern=$2 description=$3
	[ "$(line_count "$log" "$pattern")" = 0 ] ||
		die "$description must not appear"
}

run_stage()
{
	stage=$1 log=$2 fresh=$3 record=$4 completion=$5
	case "$record:$completion" in
		expected:prepare) other_record=verified; other_completion=verify ;;
		verified:verify) other_record=expected; other_completion=prepare ;;
		*) die "invalid persistence stage contract: $record/$completion" ;;
	esac
	if [ "$fresh" = 1 ]; then
		"$repo_root/scripts/run-qemu.sh" --nand-mode multiplane \
			--nand-image "$qemu_nand_image" --fresh-nand \
			--append "MTD_SMOKE=$stage" >"$log" 2>&1 || {
				cat "$log"
				die "$stage QEMU failed"
			}
	else
		"$repo_root/scripts/run-qemu.sh" --nand-mode multiplane \
			--nand-image "$qemu_nand_image" --append "MTD_SMOKE=$stage" \
			>"$log" 2>&1 || {
				cat "$log"
				die "$stage QEMU failed"
			}
	fi

	cat "$log"
	require_one "$log" \
		"^q3n multi-plane persistence $record main_digest=[0-9a-f]{64} oob_digest=[0-9a-f]{64} bbm=[0-9a-f]{8}$" \
		"^q3n multi-plane persistence $record" \
		"q3n multi-plane persistence $record record"
	require_one "$log" \
		"^q3n multi-plane persistence $completion passed$" \
		"^q3n multi-plane persistence $completion" \
		"q3n multi-plane persistence stage success"
	require_none "$log" "q3n multi-plane persistence $other_record" \
		"q3n multi-plane persistence $other_record record"
	require_none "$log" "q3n multi-plane persistence $other_completion" \
		"q3n multi-plane persistence $other_completion stage namespace"
	require_one "$log" '^MTD smoke 测试通过，关闭虚拟机$' '^MTD smoke' \
		"q3n multi-plane guest success"
	require_one "$log" \
		'^\[[[:space:]]*[0-9][0-9]*\.[0-9][0-9]*\] reboot: Power down$' \
		'reboot: Power down' "q3n multi-plane kernel powerdown record"
}

metadata_value()
{
	line=$1 field=$2
	printf '%s\n' "$line" | sed -n "s/.* $field=\\([0-9a-f][0-9a-f]*\\)\( \|$\).*/\\1/p"
}

metadata_field()
{
	line=$1 field=$2
	for token in $line; do
		case "$token" in
			"$field="*) printf '%s\n' "${token#*=}"; return 0 ;;
		esac
	done
	return 1
}

legacy_nand_image="$work_dir/media/q3n-nand.raw"
[ ! -L "$legacy_nand_image" ] ||
	die "legacy q3n NAND image must not be a symlink: $legacy_nand_image"
[ -f "$legacy_nand_image" ] ||
	die "legacy q3n NAND image is missing: $legacy_nand_image"
legacy_dir=$(CDPATH= cd -- "$(dirname -- "$legacy_nand_image")" && pwd -P)
legacy_nand_image="$legacy_dir/$(basename -- "$legacy_nand_image")"
[ "$(basename -- "$legacy_nand_image")" = q3n-nand.raw ] ||
	die "legacy q3n NAND image has an unexpected basename: $legacy_nand_image"
[ ! -L "$legacy_nand_image" ] && [ -f "$legacy_nand_image" ] ||
	die "legacy q3n NAND image is not a canonical regular file: $legacy_nand_image"

legacy_fingerprint()
{
	[ ! -L "$legacy_nand_image" ] && [ -f "$legacy_nand_image" ] ||
		return 1
	printf '%s:%s\n' "$legacy_nand_image" "$(image_fingerprint "$legacy_nand_image")"
}

legacy_prepare_before=$(legacy_fingerprint) ||
	die "legacy q3n NAND image stat failed before prepare"
printf 'q3n multi-plane persistence legacy prepare-before=%s\n' "$legacy_prepare_before"

run_stage q3n-multiplane-persist-prepare "$prepare_log" 1 expected prepare
legacy_prepare_after=$(legacy_fingerprint) ||
	die "legacy q3n NAND image stat failed after prepare"
printf 'q3n multi-plane persistence legacy prepare-after=%s\n' "$legacy_prepare_after"
[ "$legacy_prepare_after" = "$legacy_prepare_before" ] ||
	die "legacy q3n NAND image changed during prepare"

legacy_verify_before=$(legacy_fingerprint) ||
	die "legacy q3n NAND image stat failed before verify"
printf 'q3n multi-plane persistence legacy verify-before=%s\n' "$legacy_verify_before"
[ "$legacy_verify_before" = "$legacy_prepare_before" ] ||
	die "legacy q3n NAND image changed between persistence boots"
run_stage q3n-multiplane-persist-verify "$verify_log" 0 verified verify
legacy_verify_after=$(legacy_fingerprint) ||
	die "legacy q3n NAND image stat failed after verify"
printf 'q3n multi-plane persistence legacy verify-after=%s\n' "$legacy_verify_after"
[ "$legacy_verify_after" = "$legacy_verify_before" ] ||
	die "legacy q3n NAND image changed during verify"

prepare_line=$(sed 's/\r$//' "$prepare_log" | grep -E \
	'^q3n multi-plane persistence expected main_digest=[0-9a-f]{64} oob_digest=[0-9a-f]{64} bbm=[0-9a-f]{8}$')
verify_line=$(sed 's/\r$//' "$verify_log" | grep -E \
	'^q3n multi-plane persistence verified main_digest=[0-9a-f]{64} oob_digest=[0-9a-f]{64} bbm=[0-9a-f]{8}$')
prepare_main=$(metadata_field "$prepare_line" main_digest)
prepare_oob=$(metadata_field "$prepare_line" oob_digest)
prepare_bbm=$(metadata_field "$prepare_line" bbm)
verify_main=$(metadata_field "$verify_line" main_digest)
verify_oob=$(metadata_field "$verify_line" oob_digest)
verify_bbm=$(metadata_field "$verify_line" bbm)

[ "$prepare_main" = "$expected_main_digest" ] || die "unexpected prepared main digest"
[ "$prepare_oob" = "$expected_oob_digest" ] || die "unexpected prepared OOB digest"
[ "$prepare_bbm" = "$expected_bbm" ] || die "unexpected prepared BBM"
[ "$verify_main" = "$prepare_main" ] || die "persisted main digest changed after restart"
[ "$verify_oob" = "$prepare_oob" ] || die "persisted OOB digest changed after restart"
[ "$verify_bbm" = "$prepare_bbm" ] || die "persisted BBM changed after restart"

[ "$(dd if="$nand_image" bs=8 count=1 2>/dev/null)" = Q3NMEDIA ] ||
	die "q3n multi-plane NAND image magic mismatch"
logical_bytes=$(wc -c <"$nand_image")
allocated_kib=$(du -k "$nand_image" | awk '{print $1}')
[ $((allocated_kib * 1024)) -lt "$logical_bytes" ] ||
	die "q3n multi-plane NAND image is not sparse"

image_fingerprint "$nand_image" >/dev/null ||
	die "q3n multi-plane NAND image stat failed"
printf '%s\n' 'q3n multi-plane persistence smoke passed'
