#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

sh "$repo_root/tests/test_scripts.sh"
sh "$repo_root/tests/test_q3n_overlay.sh"
sh "$repo_root/tests/test_linux_patches.sh"

for script in "$repo_root"/scripts/*.sh; do
  sh -n "$script"
done
sh -n "$repo_root/scripts/lib/common.sh"
sh -n "$repo_root/rootfs/init"
sh -n "$repo_root/rootfs/profile.d/mtd.sh"

printf 'ok: smoke test passed\n'
