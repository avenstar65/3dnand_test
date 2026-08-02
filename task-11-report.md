# Task 11 report — Q3N multi-plane two-boot persistence

Base: `9bca421`.

## Contract implemented

- `q3n-multiplane-persistence-smoke.sh` uses only
  `work/media/q3n-nand-multiplane.raw`: prepare is the sole stage with
  `--fresh-nand`; verify reuses that same image.
- The wrapper accepts exactly one anchored expected/verified record, stage
  success line, guest-success line, and kernel-format `reboot: Power down`
  record for each boot.  It compares the main, raw-OOB, and BBM fields,
  requires `Q3NMEDIA`, and proves the image is sparse.
- On macOS the wrapper maps its host image path to a workspace-relative
  argument before `run-qemu` re-enters Docker.  Host-side media checks retain
  the canonical host path.  This prevents each container boot from using an
  ephemeral image outside `/workspace`.
- Guest prepare writes full 65536-byte main data and full 4096-byte raw OOB
  data in block 0, hashes both, then marks block 1 bad.  Verify rereads both
  digests, checks `MEMGETBADBLOCK`, all four BBMs, normal/raw read rejection,
  and BBT reconstruction after module reload.
- Per corrected NAND Core semantics, block 1 main-data preservation is not
  asserted: `nand_block_markbad_lowlevel()` erases before writing BBMs.
  `mtd_badblock oob-dump` provides the binary full-raw-OOB input to SHA-256.

## TDD evidence

- A temporary `BASE=9bca421` fixture carrying the new behavior test but no
  persistence wrapper failed with `multi-plane persistence host wrapper is
  missing` (exit 1).
- The host test then exposed the macOS handoff defect: it expected the
  container argument `work/media/q3n-nand-multiplane.raw` but received a host
  `/Users/...` path.  Before the fix, real verify failed with `ioctl: Input/
  output error` because prepare and verify used different ephemeral files.
- The path-mapping test, malformed/duplicate/mismatch/no-powerdown negatives,
  unknown-stage rejection, full-OOB guest fixture, and sync assertion are all
  green in the final test run.

## Real two-boot evidence

After rebuilding the rootfs, the real workflow emitted:

```text
q3n multi-plane persistence expected main_digest=944044fe482bc4e91085c15c5a923a1b9e02eac98d3bce04997d6dbecd2a5b8d oob_digest=f0f9ce8608610d597e3416195182a2d1f47d53cf00f1e72e3824a5bc3bfa7ce8 bbm=00000000
q3n multi-plane persistence prepare passed
[    2.537426] reboot: Power down
q3n multi-plane persistence verified main_digest=944044fe482bc4e91085c15c5a923a1b9e02eac98d3bce04997d6dbecd2a5b8d oob_digest=f0f9ce8608610d597e3416195182a2d1f47d53cf00f1e72e3824a5bc3bfa7ce8 bbm=00000000
q3n multi-plane persistence verify passed
[    2.233312] reboot: Power down
q3n multi-plane persistence smoke passed
```

The dedicated image is `116549226496` logical bytes with `461056` KiB
allocated, so it remains sparse.  The legacy `q3n-nand.raw` fingerprint was
unchanged before and after:

```text
82254988:116549226496:1785342127:345728
```

## Verification

- `sh tests/test_scripts.sh` — pass.
- `./scripts/shell.sh ./scripts/build-rootfs.sh` — pass.
- `./scripts/q3n-multiplane-persistence-smoke.sh` — pass (two real boots).
- `sh scripts/smoke-test.sh` — pass.
- `git diff --check` — pass.

The stale, explicitly identified `linux-mtd-qemu-dev-97337` qtest container
was stopped and is no longer present; no active QEMU process remains.
