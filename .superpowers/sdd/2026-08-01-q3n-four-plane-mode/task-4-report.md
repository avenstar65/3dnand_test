# Task 4 Report: Q3N multi-plane MMIO/PIO hardware layer

## Implementation

- Added the multi-plane ABI exactly at `0x00c4..0x00e8`: group address,
  completion masks, ECC selector, and selected-plane ECC result registers.
  Added `Q3N_CAP_MULTIPLANE` at bit 4 and MP commands 9 through 13.
- Exposed reusable low-level MMIO operations, raw READY/status waiting,
  streaming PIO helpers, and CAP reading from the existing hardware layer.
  The original wait wrapper still maps ordinary controller ERROR to `-EIO`.
- Added the standalone multi-plane hardware API with per-plane transport/ECC
  results. It writes group addressing and fixed transfer lengths, waits for
  READY, then always records DONE/FAIL before classifying completion.
- Legal partial completion preserves masks and per-plane failure status before
  returning `-EIO`. Invalid/overlapping/incomplete masks return `-EPROTO`;
  zero masks are treated as QEMU pre-validation failure (`-EINVAL` without
  ordinary ERROR, `-EIO` with it).
- Main reads read all four selected ECC records and the complete 64 KiB staging
  window even for a legal partial media failure. Global ECC-uncorrectable does
  not become a transport failure.
- Intentionally did not add either multi-plane object to the production
  Makefile; Task 8 owns conditional production linkage.

## Files

- `linux/drivers/mtd/nand/raw/qemu_3dnand_regs.h`
- `linux/drivers/mtd/nand/raw/qemu_3dnand_hw.c`
- `linux/drivers/mtd/nand/raw/qemu_3dnand_hw.h`
- `linux/drivers/mtd/nand/raw/qemu_3dnand_hw_multiplane.c`
- `linux/drivers/mtd/nand/raw/qemu_3dnand_hw_multiplane.h`
- `tests/test_q3n_hw.c`
- `tests/test_q3n_hw_multiplane.c`
- `tests/test_q3n_hw_multiplane.sh`
- `scripts/smoke-test.sh`

## RED

Command:

```sh
sh tests/test_q3n_hw_multiplane.sh
```

Observed expected failure before the implementation:

```text
clang: error: no such file or directory:
  '.../linux/drivers/mtd/nand/raw/qemu_3dnand_hw_multiplane.c'
```

The test already included the required multi-plane header/API and exercised
the intended MMIO contract; compilation failed because the implementation did
not yet exist.

## GREEN

Commands:

```sh
sh tests/test_q3n_hw_multiplane.sh
sh tests/test_q3n_hw.sh
sh scripts/smoke-test.sh
git diff --check
```

Key output:

```text
ok: Q3N multi-plane MMIO/PIO sequencing verified
ok: Q3N hardware register sequencing verified
ok: smoke test passed
```

The new fake-MMIO test records every register read/write and DATA word. It
checks exact main-read setup, DONE/FAIL-before-ECC ordering, selector 0..3,
64 KiB plane-ordered staging, 64 KiB all-`0xff` program writes, 4 KiB OOB
transfers, erase-without-DATA, timeout, protocol errors, pre-validation
failures, partial failure detail, and ECC-uncorrectable transport success.

## Self-review

- Old single-page MMIO behavior remains covered: ordinary ERROR from the old
  wait path still returns `-EIO`, and capability reading performs one CAP read.
- MP result state is zeroed before all command validation and no masks are
  fabricated on timeout or QEMU pre-validation failure.
- Valid partial READ does not return early: it retains masks, reads selected
  ECC for all planes, and consumes the full main staging window before
  returning `-EIO`.
- No Page RAID source or production object composition was modified.

## Concerns

- This task provides host fake-MMIO coverage only. QEMU ABI implementation
  and guest-level multi-plane integration remain assigned to later tasks.
