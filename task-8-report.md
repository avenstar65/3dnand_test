# Task 8 report — Q3N multi-plane NAND Core profile

## RED

- `sh tests/test_q3n_page_raid_config.sh` initially failed in the multi-plane
  profile because `qemu_3dnand_multiplane_layout.o` was absent from the
  resolved `qemu_3dnand-y` object list.
- `sh tests/test_q3n_ecc.sh` initially failed the direct plane-mask case:
  `failed_plane_mask=0x0a` left `stats.failed` at `1` instead of incrementing
  it once to `2` for the one logical page.
- A normal overlay/configure/build attempt stopped before compiling Task 8:
  the current overlay does not install Task 7's multi-plane headers and
  sources, so `qemu_3dnand_multiplane_layout.h` was unavailable. This is a
  Task 9 overlay boundary, not a claim that normal overlay support is ready.

## GREEN

- Mode-exclusive Makefile composition resolves to common-only identity,
  Page-RAID-only RAID, and the three multi-plane layout/HW/ops objects only
  for multi-plane.
- Probe now selects exactly one compile-time storage mode, validates the
  copied YTMC topology, reads CAP before page-layer creation, and hard-fails
  multi-plane without `Q3N_CAP_MULTIPLANE` using `-EOPNOTSUPP`.
- ECC recognizes either a plane failure mask or the existing logical failure
  count as one NAND-Core logical page failure. Raw callbacks retain no ECC
  accounting.
- Multi-plane uses four free OOB regions via the Task 7 pure helper; ECC
  regions remain empty, `logical_size` is the common MTD-size source, and
  the driver requires NAND Core's computed `oobavail` to be `4092`.

## Fresh kernel-build evidence

Normal overlay and multi-plane configuration were run first. Because Task 9
has not yet added the Task 7 MP files to the overlay, the following exact
temporary build-fixture files were copied from the repository into
`work/linux/linux-7.0.12/drivers/mtd/nand/raw/` only for this build:

- `qemu_3dnand_multiplane_layout.c`
- `qemu_3dnand_multiplane_layout.h`
- `qemu_3dnand_hw_multiplane.c`
- `qemu_3dnand_hw_multiplane.h`
- `qemu_3dnand_multiplane.c`
- `qemu_3dnand_multiplane.h`

The raw-NAND target compiled and linked all common and MP objects. The
kernel `modules` target then generated a fresh `qemu_3dnand.ko`; the raw
directory target alone only updates `qemu_3dnand.o` and leaves an old `.ko`.
With `Q3N_REQUIRE_KERNEL_BUILD=1` and an explicit build directory, the
compiled contract passed. It observed imports for `nand_scan_with_ids`,
`nand_cleanup`, `mtd_device_parse_register`, and `mtd_device_unregister`;
it also observed `q3n_multiplane_get_ops`, `q3n_multiplane_layout_build`,
`q3n_hw_mp_read_page`, `q3n_hw_mp_program_page`, and
`q3n_hw_mp_erase_group`. The module contains all four required multi-plane
initialization log literals and no Page RAID ops symbol.

## Verification

- `sh tests/test_q3n_page_raid_config.sh`
- `sh tests/test_q3n_multiplane_config.sh`
- `sh tests/test_q3n_ecc.sh`
- `sh scripts/smoke-test.sh`
- `git diff --check`
- Fresh multi-plane raw-NAND build, `modules`, and strict compiled contract

All listed checks passed after the GREEN changes.

## Remaining boundary

Task 9 must add the six Task 7 MP files to the normal Linux overlay before a
non-fixture multi-plane build is expected to work. Task 10 remains responsible
for guest runtime verification of the exact initialization log and geometry.

## Round 1: mode-exclusive OOB provider link fix

### RED

The common `qemu_3dnand_ecc.o` initially emitted an undefined reference to
`q3n_multiplane_oob_free_region`. A fresh identity artifact built from the
isolated `work/task8-link/identity` configuration showed that exact undefined
symbol in `nm -u qemu_3dnand.ko`; the provider object is intentionally not
linked outside multi-plane mode.

### GREEN

The MP-only OOB layout header, callback, ops table, and `q3n_ecc_init()`
selection branch are now compiled only with
`CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE`. The compiled-contract regression
checks every non-MP module has neither a definition nor an unresolved import
of the MP-only provider, and requires the MP module to define it.

Fresh isolated builds passed for identity, Page RAID 4:1, Page RAID 8:1, and
multi-plane. Each ran the raw-NAND target followed by the direct
`drivers/mtd/nand/raw/qemu_3dnand.ko` target and artifact contract. Isolated
output trees do not contain `vmlinux.o`; therefore direct module linking uses
`KBUILD_MODPOST_WARN=1` for unrelated kernel-core imports, while `nm` and the
compiled contract strictly reject the Q3N MP-only unresolved provider. A full
isolated `make modules` is not a useful Q3N signal here because it additionally
modposts unrelated UBIFS/core modules without vmlinux symbols.
