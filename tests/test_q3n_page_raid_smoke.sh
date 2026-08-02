#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Execute the guest stage resolver; do not inspect implementation text.
. "$repo_root/rootfs/profile.d/mtd.sh"
command=$(mtd_smoke_command_for q3n-page-raid-smoke) || {
  echo 'FAIL: current Page RAID guest stage does not resolve' >&2
  exit 1
}
[ "$command" = mtd_q3n_page_raid_smoke ] || {
  echo "FAIL: Page RAID guest stage resolved to $command" >&2
  exit 1
}

[ -x "$repo_root/scripts/q3n-page-raid-smoke.sh" ] || {
  echo 'FAIL: Page RAID host wrapper is missing' >&2
  exit 1
}

echo 'ok: current NAND Core Page RAID guest acceptance resolves'
