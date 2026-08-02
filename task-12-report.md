# Task 12 report — Q3N four-plane documentation and final validation

## Documentation

- Updated the register reference to the implemented capability (`CAP=0x1f`,
  including bit 4), commands 9--13, registers `0xc4..0xe8`, fixed 65536 B
  main/4096 B OOB groups, plane slice ordering, masks, selected-plane ECC,
  one READY/IRQ, RESET clearing, and functional-only limitations.
- Corrected NAND Core `markbad`: it erases before BBM/BBT update.  The document
  does not claim rollback, power-loss atomicity, timing accuracy, or parallel
  performance.
- README now indexes `--nand-mode`, the dedicated multi-plane image and
  persistence commands.

## Retained evidence

- Focused ABI, engine and Linux HW checks passed.
- Full host smoke and `git diff --check` passed before final documentation.
- Real QEMU rebuilt successfully.
- Real isolated Linux matrix built identity, RAID 2:1/4:1/8:1 and multi-plane;
  every resulting module passed the strict NAND Core contract.
- Multi-plane guest smoke passed, including erased-media verification after
  NAND Core markbad.  Two-boot persistence passed with matching main/OOB
  digests and unchanged legacy-image fingerprints.
- Page RAID source diff against `d6c2ff6` is empty.  Both required images
  exist; the legacy image was preserved.

## Current Page RAID regression

`q3n-serial-smoke.sh` is a retained pre-NAND-Core scheduler/debugfs acceptance:
its `foreground_ops`/parity-stat provider was removed with the legacy direct
MTD scheduler.  It is not a current Page RAID regression.  Task 12 adds the
behavior-tested `q3n-page-raid-smoke` path instead.  It runs RAID4 with the
dedicated fresh `work/media/q3n-page-raid4-smoke.raw`, validates implemented
`65536/4096/4095/20971520/34896609280/1664` geometry, full raw main/OOB
round-trips at block boundary, exact guest success and real powerdown, while
checking that `q3n-nand.raw` is unchanged.

Real marker:

```text
q3n page RAID smoke passed
```
