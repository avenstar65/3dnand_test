#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

mkdirs
log="$work_dir/q3n-page-raid-smoke.log"
legacy="$work_dir/media/q3n-nand.raw"
image="$work_dir/media/q3n-page-raid4-smoke.raw"

fingerprint() { stat -f '%i:%z:%m:%b' "$1" 2>/dev/null || stat -c '%i:%s:%Y:%b' "$1"; }
[ -f "$legacy" ] && [ ! -L "$legacy" ] || die "legacy image is unavailable: $legacy"
before=$(fingerprint "$legacy")
if ! "$repo_root/scripts/run-qemu.sh" --nand-mode page-raid --nand-image "$image" \
  --fresh-nand --append 'MTD_SMOKE=q3n-page-raid-smoke' >"$log" 2>&1; then
  cat "$log"; die 'q3n Page RAID smoke QEMU failed'
fi
cat "$log"
count() { sed 's/\r$//' "$log" | grep -E -c "$1" || true; }
guest='^q3n page RAID guest complete writesize=65536 oobsize=4096 oobavail=4095 erasesize=20971520 size=34896609280 blocks=1664$'
[ "$(count "$guest")" = 1 ] && [ "$(count '^q3n page RAID guest complete')" = 1 ] || die 'Page RAID guest metadata must appear exactly once'
[ "$(count '^MTD smoke 测试通过，关闭虚拟机$')" = 1 ] && [ "$(count '^MTD smoke')" = 1 ] || die 'Page RAID success marker must appear exactly once'
[ "$(count '^\[[[:space:]]*[0-9][0-9]*\.[0-9][0-9]*\] reboot: Power down$')" = 1 ] && [ "$(count 'reboot: Power down')" = 1 ] || die 'Page RAID powerdown record must appear exactly once'
[ "$before" = "$(fingerprint "$legacy")" ] || die 'Page RAID smoke changed legacy image'
echo 'q3n page RAID smoke passed'
info 'q3n Page RAID host acceptance passed'
