#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

sh "$repo_root/tests/test_scripts.sh"
sh "$repo_root/tests/test_q3n_overlay.sh"
sh "$repo_root/tests/test_q3n_controller.sh"
sh "$repo_root/tests/test_q3n_controller_mmio_harness.sh"
sh "$repo_root/tests/test_q3n_controller_mmio_cleanup.sh"
sh "$repo_root/tests/test_q3n_controller_mmio_launcher.sh"
sh "$repo_root/tests/test_q3n_controller_mmio.sh"
sh "$repo_root/tests/test_q3n_qemu_multiplane.sh"
sh "$repo_root/tests/test_q3n_qemu_multiplane_abi.sh"
sh "$repo_root/tests/test_linux_patches.sh"
sh "$repo_root/tests/test_q3n_page_raid_config.sh"
sh "$repo_root/tests/test_q3n_multiplane_config.sh"
sh "$repo_root/tests/test_q3n_addr.sh"
sh "$repo_root/tests/test_q3n_flash.sh"
sh "$repo_root/tests/test_q3n_multiplane_layout.sh"
sh "$repo_root/tests/test_q3n_multiplane.sh"
sh "$repo_root/tests/test_q3n_layout.sh"
sh "$repo_root/tests/test_q3n_page.sh"
sh "$repo_root/tests/test_q3n_hw.sh"
sh "$repo_root/tests/test_q3n_hw_multiplane.sh"
sh "$repo_root/tests/test_q3n_ecc.sh"
sh "$repo_root/tests/test_q3n_legacy.sh"
sh "$repo_root/tests/test_q3n_nand_core_contract.sh"

for script in "$repo_root"/scripts/*.sh; do
  sh -n "$script"
done
sh -n "$repo_root/scripts/lib/common.sh"
sh -n "$repo_root/rootfs/init"
sh -n "$repo_root/rootfs/profile.d/mtd.sh"

printf 'ok: smoke test passed\n'
