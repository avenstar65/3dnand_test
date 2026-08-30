#!/usr/bin/env sh
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"
mkdirs
log="$work_dir/q3n-raid5-smoke.log"
if ! "$repo_root/scripts/run-qemu.sh" --fresh-nand \
     --append "MTD_SMOKE=q3n-raid5-smoke" >"$log" 2>&1; then
  cat "$log"
  die "q3n RAID5 smoke QEMU failed"
fi
cat "$log"
grep -Fq 'q3n raid5 smoke passed' "$log" || die "q3n RAID5 marker missing"
grep -Fq 'MTD smoke 测试通过' "$log" || die "generic MTD marker missing"
info "q3n RAID5 host acceptance passed"
