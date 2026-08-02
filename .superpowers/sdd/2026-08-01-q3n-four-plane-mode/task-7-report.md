# Task 7 report: Q3N fixed four-plane page operations

## RED / GREEN record

- RED: `sh tests/test_q3n_multiplane.sh` and the added multi-plane target in
  `tests/test_q3n_page.sh` were run before implementation.  Both stopped at
  the expected absent `qemu_3dnand_multiplane.c` logical layer, so no existing
  page implementation could satisfy the new behavior by accident.
- GREEN: the new executable behavior test invokes `q3n_page_*` against fake
  `q3n_hw_mp_*` operations.  It passes mapping, read/ECC, raw, program, OOB,
  erase, BBM, free-region, and allocator-cleanup cases.  `test_q3n_page.sh`
  now compiles and runs identity, Page RAID, and multi-plane page-layer modes.

## Coverage

- Page mappings are executed and asserted for logical pages `0`, `1600`, and
  `665599`: `(die, block, page)` are respectively `(0,0,0)`, `(1,0,0)`, and
  `(1,207,1599)`.
- Four-plane ECC aggregation verifies maximum `7`, corrected-bit sum `18`,
  uncorrectable planes `1/3` as mask `0x0a`, and popcount `2`.  Transport
  `-EIO` wins over ECC aggregation; raw reads make the full group call and
  return zeroed page statistics.
- A 65536-byte all-`0xff` program still makes one group program request.
  Main failure skips OOB; OOB failure is returned after the completed main
  request without rollback.  Erase is exactly one group operation and does
  not call markbad.
- First-page OOB reads fold BBM bytes at `0/1024/2048/3072`; writes normalize
  a per-device, exactly 4096-byte scratch copy and leave the caller buffer
  unchanged.  Non-first pages are not normalized.  Free OOB regions are
  `(1,1023)`, `(1025,1023)`, `(2049,1023)`, and `(3073,1023)`.
- The allocation-failure binary seeds both scratch pointers, forces the MP
  scratch allocation to fail, and confirms cleanup releases both pointers and
  clears ops/mode.  Normal cleanup also frees each non-NULL scratch pointer.

## Verification

- `sh tests/test_q3n_multiplane.sh`
- `sh tests/test_q3n_page.sh`
- `sh tests/test_q3n_multiplane_layout.sh`
- `sh tests/test_q3n_layout.sh`
- `sh scripts/smoke-test.sh`
- `git diff --check`
- `git diff d6c2ff6 -- linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.c linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.h` (no output)

All commands above completed successfully for this task.

## Scope boundary / concern

Task 7 deliberately changes `q3n_page_layer_init()` to the typed
`(q3n, mode, raid_data_pages, topology)` contract but does not adapt the
existing production caller in `qemu_3dnand_init.c`.  That caller still uses
the prior boolean contract and is the Task 8 probe/init integration boundary,
along with Makefile object wiring and ECC/OOB-layout installation.  Therefore
a production Linux build remains intentionally deferred to Task 8; this task
only proves the independently compilable logical page layer and host smoke.
