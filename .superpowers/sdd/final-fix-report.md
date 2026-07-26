# Final Review Fix Report: One-Shot Parity PROGRAM Attempt Proof

## Status

Complete. The one-shot physical parity PROGRAM fault acceptance now proves both
halves of the intended contract:

1. D6 causes exactly one parity PROGRAM attempt, even though that attempt is
   deliberately failed.
2. The following stripe-1 D0 does not cause a later placeholder parity PROGRAM
   attempt.

No production driver or QEMU behavior changed. The withdrawn out-of-order
member finding was not addressed.

## Root Cause

The guest acceptance previously sampled `parity_writes_after_fail` only after
the deliberate stripe-0 P PROGRAM failure and pending-parity drain. Its
next-stripe assertion proved that the D0 write did not increase the counter
from that point onward, but there was no pre-fault baseline. Consequently, the
test did not prove that the failed P PROGRAM was attempted exactly once.

## Test-Driven Development Evidence

### RED

Before changing `rootfs/profile.d/mtd.sh`, added focused static requirements in
`tests/test_scripts.sh` for:

- a `parity_writes_before_fail` sample; and
- `parity_writes_after_fail == parity_writes_before_fail + 1`.

Exact command and output:

```text
$ ./scripts/smoke-test.sh
FAIL: rootfs/profile.d/mtd.sh does not contain pattern: parity_writes_after_fail.*-eq.*parity_writes_before_fail.*\+ 1
```

Exit status: `1` (expected RED). The failure was the intended missing
pre-fault/exactly-one-attempt relationship, not a setup or syntax error.

### Minimal implementation

In the one-shot physical PROGRAM fault stage of `rootfs/profile.d/mtd.sh`:

- sampled `parity_writes_before_fail` immediately before arming
  `inject_parity_program_fail`;
- sampled `parity_writes_after_fail` immediately after D6 and the
  `pending_parity == 0` drain;
- required `parity_writes_after_fail == parity_writes_before_fail + 1`;
- retained the existing stripe-1 D0 assertion that the live `parity_writes`
  counter remains equal to `parity_writes_after_fail`.

### GREEN: aggregate smoke

Exact command and output:

```text
$ ./scripts/smoke-test.sh
ok: q3n controller OOB and LDPC behavior verified
ok: script structure verified
ok: q3n media overlay mapping verified
ok: smoke test passed
```

Exit status: `0`.

### GREEN: rootfs build

Exact command and output:

```text
$ ./scripts/shell.sh ./scripts/build-rootfs.sh
==> 生成 initramfs: /workspace/work/rootfs/initramfs.cpio.gz
==> 完成: /workspace/work/rootfs/initramfs.cpio.gz
```

Exit status: `0`.

### GREEN: real serial guest acceptance

Exact command and substantive console output (terminal clear/color control
sequences emitted by SeaBIOS were omitted):

```text
$ ./scripts/q3n-serial-smoke.sh
==> 已重置物理 NAND 镜像: /workspace/work/media/q3n-nand.raw
SeaBIOS (version rel-1.17.0-0-gb52ca86e094d-prebuilt.qemu.org)

iPXE (http://ipxe.org) 00:02.0 CA00 PCI2.10 PnP PMM+3EFD19D0+3EF319D0 CA00
Press Ctrl-B to configure iPXE (PCI 00:02.0)...

Booting from ROM...

Linux MTD QEMU 开发环境已启动
可运行: /etc/profile.d/mtd.sh smoke

dev:    size   erasesize  name
mtd0: 7c6a00000 015e0000 "qemu-3dnand"
q3n serial stage: p0 continuation
p0 continuation acceptance passed
q3n serial stage: queue failure
queue failure acceptance passed
q3n serial stage: one-shot program fault
one-shot fault acceptance passed
q3n no-frontier stripe progress passed
q3n serial stage: later protected stripe
later protected stripe acceptance passed
q3n serial stage: fault disarm paths
fault disarm acceptance passed: cancel
fault disarm acceptance passed: invalid
fault disarm acceptance passed: reset
q3n serial stage: p1 over p2
[   43.236153] rcu: INFO: rcu_preempt detected stalls on CPUs/tasks:
[   43.240567] rcu: 	(detected by 0, t=38811 jiffies, g=2744, q=19 ncpus=2)
[   43.241445] rcu: INFO: Stall ended before state dump start
p1 over p2 acceptance passed: event wait
dev:    size   erasesize  name
mtd0: 7c6a00000 015e0000 "qemu-3dnand"
foreground_ops=135
parity_reads=56
parity_writes=8
protected_stripes=7
unprotected_stripes=2
failed_stripes=2
pending_parity=0
max_pending_parity=2
q3n serial smoke passed: protected=2 unprotected=2 failed=2 faults=1 max_pending=2
MTD smoke 测试通过，关闭虚拟机
[   43.484278] reboot: Power down
==> q3n serial host acceptance passed
```

Exit status: `0`.

### GREEN: diff hygiene

Exact command and output:

```text
$ git diff --check
```

Exit status: `0`; no output.

## Files Changed

- `tests/test_scripts.sh`
  - Added focused static assertions for the pre-fault parity-write sample and
    exact `before + 1` relationship.
- `rootfs/profile.d/mtd.sh`
  - Strengthened only the one-shot physical PROGRAM fault guest acceptance.
- `.superpowers/sdd/final-fix-report.md`
  - Recorded the required evidence and self-review.

No files under `linux/` or `qemu/` changed.

## Self-Review

- The baseline sample is immediately adjacent to and before
  `echo 0 > "$stats/inject_parity_program_fail"`.
- The after sample occurs only after D6 completes and
  `mtd_q3n_wait_eq "$stats/pending_parity" 0` succeeds.
- The exact-one assertion independently derives the expected value as
  `parity_writes_before_fail + 1`.
- The existing next-stripe D0 check still reads the live counter and requires
  equality with `parity_writes_after_fail`; it was not weakened or replaced.
- The existing fault, failed-stripe, unprotected-stripe, and protected-stripe
  assertions remain intact.
- The change contains no production driver/QEMU behavior modifications, no
  unrelated cleanup, and no treatment of the withdrawn out-of-order member
  finding.
- `git diff --check` reports no whitespace errors.

## Concerns

The successful serial run emitted one transient RCU stall diagnostic during
the pre-existing P1-over-P2 scheduler stage. The kernel immediately reported
that the stall ended before state dump, the stage passed, all final counters
were printed, the guest powered down normally, and host acceptance exited
zero. This task did not change that scheduler stage, driver behavior, or QEMU
behavior.
