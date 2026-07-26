# Task 4 report: guest acceptance, persistence scope, and documentation

## Status

Complete. The final static/build/guest matrix passes in the repository's Linux
container environment. The serial guest prints
`q3n no-frontier stripe progress passed`, and persistence now covers raw NAND
main/OOB bytes, bad-block markers, and bitflip overlays without claiming
Page-RAID runtime-state restoration.

## Test-driven development record

1. Baseline `./scripts/smoke-test.sh` failed because the static suite still
   required the removed `Q3N_REG_STAT_ORDER_ERRORS` symbol.
2. The Task 4 assertions were changed first. The next smoke run failed with:
   `rootfs/profile.d/mtd.sh unexpectedly contains pattern: order_errors`.
3. Guest scripts, persistence stages, and documentation were updated until
   `./scripts/smoke-test.sh` passed.
4. Runtime acceptance then exposed two stale/incorrect assertions:
   - the pre-Task-3 serial scenario counted queued parity as unprotected, while
     Task 3 now counts only completed failures;
   - the placeholder check compared `parity_writes` with a value captured
     before the deliberately failed parity PROGRAM rather than immediately
     after it.
5. After tracing both failures to their exact guest commands, the assertions
   were corrected. The final serial guest passed and printed the required
   marker.

## Implemented changes

- Replaced the old placeholder/frontier acceptance with a failed stripe-0
  parity case followed by a successful logical page-7 write/read. The scenario
  verifies stripe 0 remains unprotected and no extra placeholder parity write
  occurs.
- Removed `order_errors` debugfs and output requirements.
- Reduced persistence to two guest boots:
  - prepare raw main/OOB bytes, an uncorrectable overlay, and a bad-block
    marker;
  - verify raw main/OOB persistence, persisted overlay failure, bad-block
    persistence, and bad-block write/erase rejection.
- Removed tail replay/failure stages, persisted placeholder inspection, and
  synthetic page-state/frontier corruption fixtures.
- Removed the obsolete tail stage dispatch from the guest init script.
- Updated static assertions for the removed ABI/source concepts and the new
  guest marker.
- Updated both READMEs with the exact supported persistence statement:

  `QEMU persists raw NAND bytes and bitflip overlays.  The driver does not`

  `restore Page-RAID runtime state after reload or VM restart in this phase.`

- Completed the later-stripe KUnit state check by asserting successful parity
  completion reaches `Q3N_STRIPE_PROTECTED`.
- Recorded Tasks 1-4 and the verification matrix in
  `.superpowers/sdd/progress.md`.

## Verification matrix

- `./scripts/smoke-test.sh` — PASS; controller OOB/LDPC, script structure, and
  media overlay tests all passed.
- `./scripts/shell.sh ./scripts/build-qemu.sh` — PASS; all 2849 Ninja targets
  completed and `qemu-system-x86_64` was produced. Direct execution of
  `./scripts/build-qemu.sh` from macOS is not a valid build environment because
  the generated build files contain `/workspace` Linux-container paths.
- `./scripts/shell.sh ./scripts/build-kernel.sh` — PASS; Linux 7.0.12 bzImage
  #34 and modules, including `qemu_3dnand.ko` and
  `qemu_3dnand_test.ko`, were built and installed.
- `./scripts/shell.sh ./scripts/build-rootfs.sh` — PASS.
- `./scripts/q3n-serial-smoke.sh` — PASS; required marker, continuation,
  queue-failure, one-shot-failure, later-stripe protection, fault-disarm, and
  P1-over-P2 checks passed.
- `./scripts/q3n-persistence-smoke.sh` — PASS; prepare and verify guests passed,
  `Q3NMEDIA` magic remained valid, and the image remained sparse.
- Forbidden-symbol scan from the Task 4 checklist — PASS with no matches.
- Guest-log scan for program-order rejection messages — PASS with no matches.
- `git diff --check` — PASS.

## Persistence debugging evidence

The first raw persistence verify attempt stopped inside:

`mtd_badblock page-read raw /dev/mtd0 16384 0x3c 101 1 0xc3`

Tracing showed that module load, MTD discovery, geometry reads, and page-0 raw
main/OOB verification had completed. The persisted page-1 overlay caused the
driver to return `-EBADMSG` with zero returned data/OOB bytes. Linux
`mtdchar_read_ioctl()` treats ECC errors as readable and retries until the
request length advances, so zero progress caused an infinite guest ioctl loop;
the host QEMU process and IRQ path were not blocked.

The acceptance test now uses the single-shot OOB ioctl for the page-1 overlay
failure. It reports the same persisted ECC error without the retry loop. Page 0
still verifies exact raw main and OOB byte values.

## Self-review

- No removed frontier/order/placeholder symbols remain in the checklist source
  paths.
- Arbitrary/repeated PROGRAM bytewise-AND behavior and erase behavior remain
  covered by the host media/controller tests.
- The next-stripe D0 operation completes after prior parity failure.
- No unprotected placeholder parity write is observed.
- CRC-verified successful parity completion reaches PROTECTED in KUnit.
- Existing LDPC, BBM, OOB, scheduler, and serial MTD acceptance remains green.
- Documentation explicitly limits restart persistence to raw NAND facts.

## Concerns and environment notes

- A raw `MEMREAD` request that receives `-EBADMSG` with zero `retlen` can loop
  in Linux `mtdchar_read_ioctl()`. Task 4 avoids that path with a single-shot
  OOB ioctl; changing the driver return-length contract is a separate follow-up.
- Initial QEMU configuration could not reach Python package indexes because
  container DNS was unavailable. An already-present platform-independent
  `wheel-0.47.0-py3-none-any.whl` cache was placed in the ignored QEMU source
  wheel directory; the final full build then passed. No compiled host artifact
  or generated build output is included in the commit.
- QEMU and kernel build commands must run through `scripts/shell.sh` on macOS.

## Formal review fix: precise persisted-overlay result

The formal review identified that the persistence guest accepted any failure
from its page-1 OOB read as proof that the persisted overlay was
uncorrectable. A dedicated helper mode now verifies the observable kernel ABI
instead:

- `oob-read-uncorrectable` issues exactly one `MEMREADOOB64` read ioctl;
- it succeeds if the ioctl exposes `-1/EBADMSG`;
- it also succeeds if Linux 7.0.12 suppresses that ECC error and returns
  success with `req.length == 0`;
- it fails for every other errno and for normal success with a nonzero returned
  length, using `ENODATA` for that unexpected normal-success case.

The zero-length success case is specific enough for this driver: corrected
`-EUCLEAN` reads copy OOB data and report a nonzero length.

### RED evidence

The first focused static assertion was added before the helper implementation:

```text
$ ./scripts/smoke-test.sh
FAIL: rootfs/helpers/mtd_badblock.c does not contain pattern: oob-read-ebadmsg
```

The initial strict `EBADMSG`-only implementation made host smoke and rootfs
build pass, but the real persistence verify guest failed before its marker.
Temporary diagnostic output showed:

```text
ioctl: No data available
MTD smoke 测试失败，进入 shell
错误: q3n-persist-verify marker missing
```

This was the helper's intentional `ENODATA` for unexpected ioctl success.
Inspection of Linux 7.0.12 `mtdchar_readoob()` confirmed why:
`mtd_read_oob()` returns `-EBADMSG`, but mtdchar explicitly returns ioctl
success for `mtd_is_bitflip_or_eccerr(ret)`. Because the driver copied no OOB
bytes for the uncorrectable page, the returned length is zero. The previous
generic helper failed only because its later short-length check converted that
result to `EIO`.

After revising the mode name and contract to the observable ABI, the renamed
static assertion was again verified RED before production edits:

```text
$ ./scripts/smoke-test.sh
FAIL: rootfs/helpers/mtd_badblock.c does not contain pattern: oob-read-uncorrectable
```

### GREEN evidence

```text
$ ./scripts/smoke-test.sh
ok: q3n controller OOB and LDPC behavior verified
ok: script structure verified
ok: q3n media overlay mapping verified
ok: smoke test passed

$ ./scripts/shell.sh ./scripts/build-rootfs.sh
==> 生成 initramfs: /workspace/work/rootfs/initramfs.cpio.gz
==> 完成: /workspace/work/rootfs/initramfs.cpio.gz

$ ./scripts/q3n-persistence-smoke.sh
q3n persistence prepare passed
q3n persistence verify passed
==> q3n persistence smoke passed

$ git diff --check
[exit 0, no output]
```

Self-review confirmed the mode accepts only the two uncorrectable signatures,
does not mistake raw-mode setup errors for read errors, invokes one read ioctl,
preserves every existing helper mode, and leaves no diagnostic instrumentation
or `oob-read-ebadmsg` name behind.
