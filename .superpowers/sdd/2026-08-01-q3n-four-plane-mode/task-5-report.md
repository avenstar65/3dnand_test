# Task 5 Report: QEMU fixed four-plane group engine

## Scope and implementation

- Added a standalone, non-QOM `q3n-multiplane` engine.  Its public header
  depends only on standard C integer and boolean types, defines the Task 5
  topology/transfer constants, and exposes the five callback-driven group
  operations.
- Every operation performs all address and transfer validation before invoking
  a media callback.  Invalid requests clear the supplied result and leave both
  masks at zero.
- The physical block formula is exactly
  `(die * 4 + plane) * 247 + block`; every valid group visits planes 0, 1, 2,
  and 3 in that order.  A media failure records its fail bit but never stops
  the later plane callbacks.
- Main and OOB reads begin erased and restore an unsuccessful slice to `0xff`
  after a callback failure.  Main reads preserve per-plane ECC results on
  successful non-raw reads.  Raw reads forward `raw=true` and leave all four
  ECC results zero.  ECC status is never converted into a media fail bit.
- Registered the executable host behavior test in `scripts/smoke-test.sh`.

## Files

- `qemu/hw/mtd/q3n-multiplane.c`
- `qemu/hw/mtd/q3n-multiplane.h`
- `tests/test_q3n_qemu_multiplane.c`
- `tests/test_q3n_qemu_multiplane.sh`
- `scripts/smoke-test.sh`

## RED evidence

The behavior test was written before the engine interface or implementation.
It compiles and runs the callback-driven executor when available; the first
run failed because the public engine header did not exist:

```sh
sh tests/test_q3n_qemu_multiplane.sh
```

```text
tests/test_q3n_qemu_multiplane.c:7:10: fatal error: 'q3n-multiplane.h' file not found
    7 | #include "q3n-multiplane.h"
      |          ^~~~~~~~~~~~~~~~~~
1 error generated.
```

The initial GREEN attempt also caught a real callback-boundary defect: the
fake failed read intentionally overwrote its destination and the test reported
`failed read remains erased: got 0, want 255`.  The executor now restores a
failed main or OOB slice to `0xff` after the callback.

## GREEN evidence

```sh
sh tests/test_q3n_qemu_multiplane.sh
sh tests/test_q3n_controller.sh
sh tests/test_q3n_overlay.sh
sh scripts/smoke-test.sh
git diff --check
```

The focused commands print:

```text
ok: Q3N QEMU multi-plane engine verified
ok: q3n controller OOB and LDPC behavior verified
ok: q3n media overlay mapping verified
```

The complete smoke suite exits zero and ends with `ok: smoke test passed`.

## Boundary coverage and self-review

- Literal mapping coverage includes `die=0, block=0` to
  `0,247,494,741` and `die=1, block=207` to
  `1195,1442,1689,1936`.
- All five operations prove four callbacks in plane order, expected slice
  placement, and continuation after a chosen plane fails.  Completed masks
  are verified to be disjoint and to union to `0x0f`.
- Tests reject die `2`, block `208`, page `1600`, short main/OOB transfer
  lengths, and verify zero callbacks and zero masks for invalid requests.
  ERASE accepts only zero transfer lengths.
- The engine has no QOM, MMIO, IRQ, meson, Page RAID, or Linux driver changes.
  It does not issue completion interrupts or mutate global controller state.

## Follow-up boundary

The requested compile-time equality assertions between this engine's constants
and `q3n-nand.h` are deliberately not added here: they require including the
engine in the QEMU controller integration and belong to Task 6, together with
QOM/MMIO/meson wiring.  This Task 5 commit keeps the engine independently
buildable without that dependency.
