#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

mkdirs
log="$work_dir/q3n-ubifs-smoke.log"
nand_image="$work_dir/media/q3n-ubifs-smoke.raw"

cleanup() {
  rm -f "$nand_image"
}

trap cleanup 0 HUP INT TERM
if ! "$repo_root/scripts/run-qemu.sh" --fresh-nand \
     --nand-image work/media/q3n-ubifs-smoke.raw \
     --append "MTD_SMOKE=q3n-ubifs-smoke" >"$log" 2>&1; then
  cat "$log"
  die "q3n pSLC RAID1 UBIFS QEMU failed"
fi

cat "$log"
grep -Fq 'q3n pSLC RAID1 UBIFS smoke passed' "$log" ||
  die "q3n UBIFS marker missing"
grep -Fq 'MTD smoke 测试通过' "$log" || die "generic MTD marker missing"
info "q3n pSLC RAID1 UBIFS host acceptance passed"
