#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

mkdirs
log="$work_dir/q3n-kunit-smoke.log"

if ! "$repo_root/scripts/run-qemu.sh" --fresh-nand \
     --append "MTD_SMOKE=q3n-kunit-smoke" >"$log" 2>&1; then
  cat "$log"
  die "q3n KUnit smoke QEMU failed"
fi

cat "$log"
for marker in 'q3n KUnit smoke passed' 'MTD smoke 测试通过'; do
  grep -Fq "$marker" "$log" || die "q3n KUnit marker missing: $marker"
done

info "q3n KUnit host acceptance passed"
