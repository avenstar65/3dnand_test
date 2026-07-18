#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

mkdirs
prepare_log="$work_dir/q3n-persist-prepare.log"
verify_log="$work_dir/q3n-persist-verify.log"
nand_image="$work_dir/media/q3n-nand.raw"

run_stage() {
  log=$1
  marker=$2
  fresh=$3
  stage=$4

  if [ "$fresh" -eq 1 ]; then
    if ! "$repo_root/scripts/run-qemu.sh" --fresh-nand \
         --append "MTD_SMOKE=$stage" >"$log" 2>&1; then
      cat "$log"
      die "$stage QEMU failed"
    fi
  elif ! "$repo_root/scripts/run-qemu.sh" \
       --append "MTD_SMOKE=$stage" >"$log" 2>&1; then
    cat "$log"
    die "$stage QEMU failed"
  fi
  cat "$log"
  grep -q "$marker" "$log" || die "$stage marker missing"
}

run_stage "$prepare_log" 'q3n persistence prepare passed' 1 \
  q3n-persist-prepare
run_stage "$verify_log" 'q3n persistence verify passed' 0 \
  q3n-persist-verify

[ "$(dd if="$nand_image" bs=8 count=1 2>/dev/null)" = "Q3NMEDIA" ] || \
  die "q3n NAND image magic mismatch"
logical_bytes=$(wc -c < "$nand_image")
allocated_kib=$(du -k "$nand_image" | awk '{print $1}')
[ $((allocated_kib * 1024)) -lt "$logical_bytes" ] || \
  die "q3n NAND image is not sparse"

run_stage "$work_dir/q3n-tail-prepare.log" 'q3n tail prepare passed' 1 \
  q3n-tail-prepare
run_stage "$work_dir/q3n-tail-verify.log" 'q3n tail verify passed' 0 \
  q3n-tail-verify

run_stage "$work_dir/q3n-tail-fail-prepare.log" \
  'q3n tail failure prepare passed' 1 q3n-tail-fail-prepare
run_stage "$work_dir/q3n-tail-fail-verify.log" \
  'q3n tail failure verify passed' 0 q3n-tail-fail-verify

# Image v1 uses block-state offset 4096 and page-state offset 12000 for the
# fixed 1976-block geometry.  The preceding failure case leaves block 0 at
# frontier 7; marking page-state[7] present must therefore be rejected.
invalid_image="$work_dir/media/q3n-invalid-state.raw"
invalid_log="$work_dir/q3n-invalid-state.log"
rm -f "$invalid_image"
dd if="$nand_image" of="$invalid_image" bs=4096 count=800 \
  conv=notrunc 2>/dev/null
dd if=/dev/zero of="$invalid_image" bs=1 count=0 seek="$logical_bytes" \
  2>/dev/null
printf '\001' | dd of="$invalid_image" bs=1 seek=12007 conv=notrunc \
  2>/dev/null
invalid_image_arg=${invalid_image#"$repo_root/"}
if "$repo_root/scripts/run-qemu.sh" --nand-image "$invalid_image_arg" \
     --append "MTD_SMOKE=1" >"$invalid_log" 2>&1; then
  cat "$invalid_log"
  die "inconsistent q3n image unexpectedly accepted"
fi
grep -q 'inconsistent page state/frontier' "$invalid_log" || {
  cat "$invalid_log"
  die "inconsistent q3n image rejection reason missing"
}

info "q3n persistence smoke passed"
