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

info "q3n persistence smoke passed"
