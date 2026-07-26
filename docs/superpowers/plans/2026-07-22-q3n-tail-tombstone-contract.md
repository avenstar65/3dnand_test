# Q3N Tail Tombstone Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reconcile tail-replay persistence tests and tombstone reason codes with the approved unprotected-tombstone contract.

**Architecture:** Tail replay must classify the source of parity reconstruction failure, persist that reason in the hidden P-page tombstone, and continue registration when the tombstone program succeeds. Guest and host persistence tests will verify counters, reason bytes, exact serial-frontier behavior through a successful next D0 program, and a separately corrupted persisted frontier fixture.

**Tech Stack:** Linux MTD driver C, KUnit, BusyBox shell guest tests, QEMU persistent NAND image.

## Global Constraints

- A lost member during tail replay registers the MTD successfully and persists an UNPROTECTED tombstone.
- Failed and unprotected accounting increment; protected accounting does not.
- The hidden P page advances the frontier to 8, proven by accepting exactly the next stripe D0.
- Member read, metadata/CRC, LDPC uncorrectable, and parity-program causes retain distinct reason codes.
- A tombstone program failure remains an operation error and cannot claim a persisted reason.
- Use TDD RED/GREEN and run persistence, smoke, kernel, rootfs, OOB, serial, KUnit, helper, and style verification.

---

### Task 1: Tail persistence contract tests

**Files:**
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `scripts/q3n-persistence-smoke.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: debugfs RAID counters, the v2 NAND image layout, and logical-to-physical serial mapping.
- Produces: `mtd_q3n_tail_fail_verify` success marker and a genuinely inconsistent image fixture based on frontier 8.

- [x] **Step 1: Write failing static and guest assertions**

Require `mtd_q3n_tail_fail_verify` to load the MTD, assert `raid_failed=1`, `failed_stripes=1`, `unprotected_stripes=1`, `protected_stripes=0`, and successfully write logical page 7. Read the hidden P-page MEMBER_READ reason directly from the host NAND image, because parity pages are intentionally not addressable through the logical MTD. Change the host fixture to rewind block 0's frontier header to 8 while page-state[8] remains present.

- [x] **Step 2: Verify RED**

Run `./scripts/smoke-test.sh`; expect failure because the old probe-failure strings remain and new tail-tombstone markers are absent.

- [x] **Step 3: Implement the guest/host contract**

Replace the old no-MTD expectation with successful replay assertions. Update the invalid fixture at block-state offset 4096 and page-state offset 12008.

- [x] **Step 4: Verify the static gate is GREEN**

Run `./scripts/smoke-test.sh`; expect exit 0.

### Task 2: Failure-reason classification

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`

**Interfaces:**
- Consumes: `struct q3n_ecc_result`, parity member reads, metadata validation, parity commit result.
- Produces: validated `Q3N_UNPROTECTED_LDPC_UNCORRECTABLE` and `Q3N_UNPROTECTED_PARITY_PROGRAM` tombstone reasons and a reason output from tail parity append.

- [x] **Step 1: Write failing KUnit/static assertions**

Extend the tombstone KUnit test to round-trip every supported reason and reject values outside the enum. Require tail append to return the classified reason and require explicit ECC-uncorrectable handling.

- [x] **Step 2: Verify RED**

Apply the overlay and build the focused modules; expect compilation failure for the missing enum values/classification interface.

- [x] **Step 3: Implement minimal classification**

Classify transport read failures as MEMBER_READ, ECC status as LDPC_UNCORRECTABLE, metadata/CRC validation as INVALID_METADATA, and a normal parity commit failure as PARITY_PROGRAM. Persist the classified reason only when the subsequent tombstone program succeeds; propagate tombstone-program errors.

- [x] **Step 4: Verify focused tests are GREEN**

Apply the overlay and build the driver/KUnit modules; expect exit 0, then boot KUnit and expect all cases to pass.

### Task 3: Kernel-level OOB bounds negative test

**Files:**
- Modify: `rootfs/helpers/mtd_badblock.c`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: legacy `MEMREADOOB64`, whose encoded start carries the per-page OOB offset into `mtdchar_readoob`.
- Produces: `oob-read-unchecked`, used only to prove that the kernel ioctl rejects offset 128 rather than relying on helper validation.

- [x] **Step 1: Write the failing static/guest assertion**

Require `oob-read-unchecked place DEVICE PAGE_OFFSET 128 1` and a distinct `kernel OOB bounds rejection passed` guest marker.

- [x] **Step 2: Verify RED**

Run `./scripts/smoke-test.sh`; expect the missing helper command assertion to fail.

- [x] **Step 3: Implement the unchecked ioctl probe**

Parse mode, alignment, and allocation safely but deliberately bypass logical OOB range validation before issuing `MEMREADOOB64`; return the ioctl error to the shell.

- [x] **Step 4: Verify GREEN**

Build the helper with `-Wall -Wextra -Werror`, rebuild rootfs, and run the fresh OOB guest matrix; expect the kernel-rejection marker.

### Task 4: Full verification, report, and commit

**Files:**
- Modify: `.superpowers/sdd/task-5-report.md`

**Interfaces:**
- Consumes: all Task 1-3 outputs.
- Produces: verification evidence and one review-fix commit.

- [x] **Step 1: Run complete verification**

Run static smoke, overlay, full kernel build, rootfs build, persistence smoke, fresh OOB/BBM guest matrix, KUnit, serial smoke, helper `-Werror`, checkpatch, and `git diff --check`.

- [x] **Step 2: Append exact RED/GREEN evidence to the report**

Record failure messages, final build number, guest markers, KUnit totals, persistence result, and any deferred concerns.

- [ ] **Step 3: Commit**

Stage tracked implementation/test files and commit with `fix: align q3n tail replay with tombstones`.
