# Q3N Remove Program Frontier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove page-order enforcement and per-page program state from QEMU and the Linux driver while preserving LDPC, BBM, MTD ECC, and runtime Page-RAID behavior.

**Architecture:** QEMU persists main/OOB in complemented sparse encoding plus raw bitflip overlays; PROGRAM decodes stored bytes, applies NAND `old & new`, then re-encodes them. The Linux driver schedules requests by class only and uses a per-stripe D0..D6 completion bitmap solely to decide when parity may be built. No layer maintains a program frontier, writes an unprotected tombstone, or restores RAID runtime state after restart.

**Tech Stack:** QEMU C/BlockBackend, Linux 7.0.12 MTD raw NAND driver, KUnit, POSIX shell guest smoke tests.

## Global Constraints

- Physical page remains 16 KiB main plus 1664 B physical OOB.
- Physical OOB[0] is BBM; physical OOB[1..1536] is 16 × 96 B LDPC; physical OOB[1537..1663] maps to logical OOB[1..127].
- ECC remains 1 KiB per step, 16 steps per page, strength 40 bits per step.
- QEMU and the driver must not save, expose, enforce, or restore `next_prog_page`.
- QEMU must not save `ERASED/PRESENT/LOST` per-page state.
- Repeated PROGRAM succeeds and stores `old_byte & incoming_byte`.
- Sparse page slots use complemented encoding: backend `0x00` represents physical NAND `0xff`; overlays are not complemented.
- RAID runtime state is not restored after driver reload or VM restart in this phase.
- No UNPROTECTED tombstone is written.
- Do not modify generic MTD, UBI, or UBIFS code.

---

## File Structure

- `qemu/hw/mtd/q3n-media.c`: version-2 media layout, raw page/OOB/overlay I/O, NAND merge-program and erase.
- `qemu/include/hw/mtd/q3n-media.h`: media API without frontier or lost-page state.
- `qemu/hw/mtd/q3n-nand.c`: controller LDPC/OOB behavior and block BBM status; no order checks.
- `qemu/include/hw/mtd/q3n-nand.h`: MMIO ABI without next-page/order-error registers.
- `qemu/hw/mtd/q3n-media-overlay.h`: small pure merge/erased helpers shared with host tests.
- `tests/test_q3n_overlay.c`: host tests for NAND merge and erased-codeword detection.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c`: foreground/P1/P2 priority scheduler without frontier readiness.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_map.c`: address mapping only; remove frontier replay helpers.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`: request and RAID types without block frontier/tombstone types.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`: no frontier restore/advance, no tombstone worker, runtime-only RAID publication.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`: data/parity metadata only; remove tombstone codec.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`: scheduler and RAID tests matching the new semantics.
- `linux/drivers/mtd/nand/raw/qemu_3dnand.h`: Linux copy of the reduced MMIO ABI.
- `rootfs/profile.d/mtd.sh`: guest acceptance for next-stripe progress and no tombstone.
- `scripts/q3n-persistence-smoke.sh`: remove unsupported RAID replay/frontier fixtures; retain raw media persistence checks only.
- `tests/test_scripts.sh`, `qemu/README.md`, `README.md`: update static assertions and documented scope.

---

### Task 1: Make QEMU media stateless and merge-programmed

**Files:**
- Modify: `qemu/hw/mtd/q3n-media-overlay.h`
- Modify: `tests/test_q3n_overlay.c`
- Modify: `qemu/hw/mtd/q3n-media.c`
- Modify: `qemu/include/hw/mtd/q3n-media.h`
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `qemu/include/hw/mtd/q3n-nand.h`

**Interfaces:**
- Produces: `q3n_media_merge_program(uint8_t *stored, const uint8_t *incoming, size_t length)`.
- Produces: `q3n_media_is_erased(const uint8_t *data, size_t length)`.
- Produces: `q3n_media_invert(uint8_t *data, size_t length)` for page-slot encode/decode.
- Changes: `q3n_media_get_block_status()` returns only `status`; it has no `next_page` output.
- Removes: `q3n_media_next_prog_page()` and `q3n_media_inject_loss()`.
- Preserves: `q3n_media_program_page()`, `q3n_media_read_page()`, `q3n_media_erase_block()`, and bitflip overlay APIs.

- [ ] **Step 1: Add failing host tests for NAND merge and erased detection**

Add these focused cases to `tests/test_q3n_overlay.c`:

```c
static void test_merge_program_is_bitwise_and(void)
{
    uint8_t stored[] = { 0xff, 0xf0, 0x55, 0x00 };
    const uint8_t incoming[] = { 0x0f, 0xcc, 0xaa, 0xff };
    const uint8_t expected[] = { 0x0f, 0xc0, 0x00, 0x00 };

    q3n_media_merge_program(stored, incoming, sizeof(stored));
    assert(!memcmp(stored, expected, sizeof(stored)));
}

static void test_erased_detection_reads_bytes(void)
{
    uint8_t bytes[32];

    memset(bytes, 0xff, sizeof(bytes));
    assert(q3n_media_is_erased(bytes, sizeof(bytes)));
    bytes[17] = 0xfe;
    assert(!q3n_media_is_erased(bytes, sizeof(bytes)));
}

static void test_sparse_zero_decodes_as_erased(void)
{
    uint8_t stored[32] = { 0 };

    q3n_media_invert(stored, sizeof(stored));
    assert(q3n_media_is_erased(stored, sizeof(stored)));
}
```

Call all three tests from `main()`.

- [ ] **Step 2: Run the host test and verify RED**

Run:

```bash
./tests/test_q3n_overlay.sh
```

Expected: compilation fails because `q3n_media_merge_program` and
`q3n_media_is_erased` are not defined.

- [ ] **Step 3: Add the pure byte helpers**

Add to `qemu/hw/mtd/q3n-media-overlay.h`:

```c
static inline void q3n_media_merge_program(uint8_t *stored,
                                           const uint8_t *incoming,
                                           size_t length)
{
    size_t i;

    for (i = 0; i < length; i++)
        stored[i] &= incoming[i];
}

static inline bool q3n_media_is_erased(const uint8_t *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; i++) {
        if (data[i] != 0xff)
            return false;
    }
    return true;
}

static inline void q3n_media_invert(uint8_t *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; i++)
        data[i] = ~data[i];
}
```

- [ ] **Step 4: Run the host test and verify GREEN**

Run: `./tests/test_q3n_overlay.sh`

Expected: `ok: q3n media overlay mapping verified`.

- [ ] **Step 5: Remove block/page state from Q3NMEDIA v2**

In `qemu/hw/mtd/q3n-media.c`:

- remove `Q3N_MEDIA_PAGE_*`, `block_state_*`, `page_state_*`,
  `next_prog_page`, and `page_state`;
- calculate `page_slots_offset` immediately after the 4096-byte header;
- keep page slots aligned to 4096 and keep overlays after aligned page slots;
- reject an existing development-v2 header whose obsolete state offsets/lengths
  do not match the new zero-valued reserved fields;
- do not read, validate, write, allocate, or free page state arrays;
- derive bad-block state from page-0 BBM when checking a block;
- replace the first four reserved header bytes with
  `uint32_t page_slot_encoding`, define
  `Q3N_MEDIA_PAGE_SLOT_COMPLEMENTED = 1`, write/validate value 1, and shrink
  `reserved` from 3968 to 3964 bytes;
- make erase zero/discard every encoded page slot in the block and zero the
  block's overlay slots.

The revised header fields remain reserved and zero so the 4096-byte header size
does not change:

```c
h.block_state_offset = 0;
h.block_state_length = 0;
h.page_state_offset = 0;
h.page_state_length = 0;
```

- [ ] **Step 6: Implement read-modify-AND PROGRAM**

Replace the direct slot overwrite in `q3n_media_program_page()` with:

```c
ret = blk_pread(m->blk, slot, m->page_stride, storage, 0);
if (ret < 0)
    goto out;

q3n_media_invert(storage, m->page_stride);
q3n_media_merge_program(storage, data, m->page_size);
if (physical_oob)
    q3n_media_merge_program(storage + m->page_size, physical_oob,
                            m->physical_oob_size);

q3n_media_invert(storage, m->page_stride);
ret = blk_pwrite(m->blk, slot, m->page_stride, storage, 0);
```

Do not compare `page` with a frontier and do not reject an already programmed
slot. `q3n_media_read_page()` must invert encoded main/OOB immediately after
`blk_pread()`. Direct BBM reads/writes must also translate the single encoded
byte. Preserve bounds and BBM checks.

- [ ] **Step 7: Remove controller frontier/lost semantics**

In `q3n-nand.c` and both ABI headers:

- delete `q3n_check_program_order`, `stat_order_errors`,
  `Q3N_REG_STAT_ORDER_ERRORS`, and `Q3N_REG_BLOCK_NEXT_PAGE`;
- delete `q3n_media_inject_loss`; retain the compatibility
  `Q3N_FAULT_INJECT_DATA_LOSS` command but implement `q3n_inject_data_loss()`
  by calling `q3n_media_inject_bitflips(media, block, page, 0,
  Q3N_FAULT_REGION_MAIN, 0, Q3N_ECC_STRENGTH + 1)`;
- set the erased flag passed to `q3n_decode_ldpc()` by checking the raw main and
  physical OOB buffers with `q3n_media_is_erased`;
- make `GET_BLOCK_STATUS` report BBM-derived `Q3N_BLOCK_STATUS_BAD` only.

- [ ] **Step 8: Verify QEMU code and host tests**

Run:

```bash
./scripts/smoke-test.sh
./scripts/build-qemu.sh
```

Expected: shell/static tests pass and QEMU builds without references to
`next_prog_page`, `page_state`, `stat_order_errors`, or
`q3n_media_inject_loss`.

- [ ] **Step 9: Commit Task 1**

```bash
git add qemu/hw/mtd/q3n-media-overlay.h tests/test_q3n_overlay.c \
  qemu/hw/mtd/q3n-media.c qemu/include/hw/mtd/q3n-media.h \
  qemu/hw/mtd/q3n-nand.c qemu/include/hw/mtd/q3n-nand.h
git commit -m "feat: remove q3n media program frontier"
```

---

### Task 2: Remove driver frontier scheduling

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_map.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`

**Interfaces:**
- Changes: `struct q3n_request` contains class, op, and page identity but no
  `block_state` pointer.
- Preserves: scheduler class priority `FOREGROUND > PARITY_READ > PARITY_WRITE`.
- Removes: `struct q3n_block_state`, `q3n_program_order_ready()`, and
  `q3n_replay_serial_frontier()`.

- [ ] **Step 1: Replace frontier KUnit cases with order-independent scheduling**

Delete `q3n_replay_serial_frontier_test` and `q3n_program_order_test`. Add:

```c
static void q3n_scheduler_does_not_gate_program_page_test(struct kunit *test)
{
    struct q3n_sched sched;
    struct q3n_request p8 = {
        .class = Q3N_REQ_FOREGROUND,
        .op = Q3N_REQ_PROGRAM,
        .page = 8,
    };

    q3n_sched_init(&sched);
    KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &p8), 0);
    KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &p8), 0);
}

static void q3n_next_stripe_data_preempts_previous_parity_test(struct kunit *test)
{
    struct q3n_sched sched;
    struct q3n_request parity = {
        .class = Q3N_REQ_PARITY_WRITE,
        .op = Q3N_REQ_PROGRAM,
        .page = 7,
    };
    struct q3n_request next_data = {
        .class = Q3N_REQ_FOREGROUND,
        .op = Q3N_REQ_PROGRAM,
        .page = 8,
    };

    q3n_sched_init(&sched);
    KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &parity), 0);
    KUNIT_ASSERT_EQ(test, q3n_sched_enqueue(&sched, &next_data), 0);
    KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &next_data), 0);
    KUNIT_EXPECT_EQ(test, q3n_sched_try_start(&sched, &parity), 0);
}
```

- [ ] **Step 2: Boot the KUnit kernel and verify RED**

Run:

```bash
./scripts/build-kernel.sh
./scripts/run-qemu.sh --fresh-nand --append "MTD_SMOKE=1" \
  > work/q3n-kunit-frontier-red.log 2>&1 || true
grep -E "not ok.*q3n_scheduler_does_not_gate_program_page" \
  work/q3n-kunit-frontier-red.log
```

Expected: the new scheduler case is reported `not ok` because the existing
frontier code rejects page 8 without a matching `next_prog_page`.

- [ ] **Step 3: Simplify scheduler readiness**

In `qemu_3dnand_sched.c`:

- make every queued request ready;
- remove frontier-dependency scans;
- remove `-ESTALE` and `-ERANGE` program branches;
- retain queue validity checks, foreground/P1/P2 priority, cancellation,
  reservation accounting, and wakeup sequencing.

The selection path becomes:

```c
next = list_first_entry_or_null(&sched->foreground_queue,
                                struct q3n_request, node);
if (!next)
    next = list_first_entry_or_null(&sched->parity_read_queue,
                                    struct q3n_request, node);
if (!next)
    next = list_first_entry_or_null(&sched->parity_write_queue,
                                    struct q3n_request, node);
```

- [ ] **Step 4: Remove driver frontier types and helpers**

Delete:

- `struct q3n_block_state` and `q3n_request.block_state`;
- `q3n_program_order_ready()` and `q3n_replay_serial_frontier()`;
- `q3n->program_state` allocation and all success/erase frontier assignments;
- `Q3N_REG_BLOCK_NEXT_PAGE`, `Q3N_REG_STAT_ORDER_ERRORS`, and the debugfs
  `order_errors` file;
- `next_page` from `qemu_3dnand_get_phys_block_status_locked()`.

Keep block status for BBM only. A newly loaded driver initializes RAID runtime
state as unprotected and performs no page/frontier replay.

- [ ] **Step 5: Verify driver scheduling tests**

Run:

```bash
./scripts/build-kernel.sh
rg -n "next_prog_page|program_state|program_order|serial_frontier|order_errors" \
  linux/drivers/mtd/nand/raw
```

Expected: kernel build succeeds and `rg` returns no matches.

- [ ] **Step 6: Commit Task 2**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_map.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c
git commit -m "feat: remove q3n driver program frontier"
```

---

### Task 3: Remove tombstones and keep RAID runtime-only

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Preserves: `q3n_open_stripe_update()`,
  `q3n_open_stripe_queue_parity()`, `q3n_build_manifest()`, and verified RAID
  recovery.
- Removes: `q3n_unprotected_tombstone`,
  `q3n_pack_unprotected_oob()`, and `q3n_unpack_unprotected_oob()`.
- Produces: failed parity preparation completes the in-memory stripe as
  `Q3N_STRIPE_UNPROTECTED` without queuing a P-page PROGRAM.

- [ ] **Step 1: Add a failing no-tombstone source assertion and RAID KUnit case**

In `tests/test_scripts.sh`, add:

```sh
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'tombstone'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c 'tombstone'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h 'TOMBSTONE'
```

Remove the tombstone codec round-trip KUnit test. Extend the open-stripe test
so failure leaves runtime state unprotected and a later stripe can
independently queue parity:

```c
q3n_open_stripe_complete_parity(&first, false);
KUNIT_EXPECT_EQ(test, first.state, Q3N_STRIPE_UNPROTECTED);

for (slot = 0; slot < Q3N_RAID_MAX_DATA_PAGES; slot++)
    KUNIT_ASSERT_EQ(test, q3n_open_stripe_update(&second, slot, data,
                                                 sizeof(data)), 0);
KUNIT_EXPECT_EQ(test, q3n_open_stripe_queue_parity(&second), 0);
KUNIT_EXPECT_EQ(test, second.state, Q3N_STRIPE_PARITY_QUEUED);
```

- [ ] **Step 2: Verify RED against tombstone worker state**

Run: `./scripts/smoke-test.sh`

Expected: FAIL because the driver still contains tombstone types and worker
paths.

- [ ] **Step 3: Delete the tombstone format and codec**

Remove from `qemu_3dnand_priv.h` and `qemu_3dnand_raid.c`:

- `Q3N_RAID_TOMBSTONE_MAGIC`;
- `enum q3n_unprotected_reason`;
- `struct q3n_unprotected_tombstone`;
- pack/unpack functions and tombstone CRC helper.

- [ ] **Step 4: Change parity failure to runtime-only state**

In `qemu_3dnand_main.c`:

- remove `tombstone` and `tombstone_reason` from parity work;
- remove `qemu_3dnand_program_unprotected_tombstone_locked()`;
- when metadata validation, member read, LDPC decode, rebuild, or parity
  PROGRAM fails, increment the existing failure/unprotected counters exactly
  once, release reservations/barriers, and finish without programming P;
- do not let a failed or pending P block foreground data from another stripe;
- publish PROTECTED only after parity main+manifest PROGRAM succeeds.

Add `bool failure_accounted` to
`struct qemu_3dnand_parity_work`. Use this exact helper to avoid double
accounting:

```c
static void qemu_3dnand_account_unprotected(
		struct qemu_3dnand_parity_work *parity)
{
	if (parity->failure_accounted)
		return;
	parity->failure_accounted = true;
	parity->q3n->raid_failed++;
	atomic64_inc(&parity->q3n->failed_stripes);
	atomic64_inc(&parity->q3n->unprotected_stripes);
	parity->q3n->parity_index[
		qemu_3dnand_parity_index(parity->q3n, parity->block,
					  parity->stripe)].valid = false;
}
```

Call it from every terminal parity failure path before
`qemu_3dnand_finish_parity_work()`. Cancellation caused by block erase remains
stale/cancelled rather than failed and must not call this helper.

- [ ] **Step 5: Verify no tombstone implementation remains**

Run:

```bash
./scripts/build-kernel.sh
rg -n "tombstone|Q3N_UNPROTECTED_" linux/drivers/mtd/nand/raw
```

Expected: kernel build succeeds and `rg` returns no matches.

- [ ] **Step 6: Commit Task 3**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c tests/test_scripts.sh
git commit -m "feat: make q3n unprotected stripes runtime only"
```

---

### Task 4: Update guest acceptance, persistence scope, and documentation

**Files:**
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `rootfs/init`
- Modify: `scripts/q3n-persistence-smoke.sh`
- Modify: `tests/test_scripts.sh`
- Modify: `qemu/README.md`
- Modify: `README.md`
- Modify: `.superpowers/sdd/progress.md`

**Interfaces:**
- Produces guest marker: `q3n no-frontier stripe progress passed`.
- Retains persistence coverage for raw main/OOB/BBM/overlay bytes.
- Removes restart replay, invalid-frontier fixture, tail tombstone, and
  `order_errors` acceptance requirements.

- [ ] **Step 1: Change static tests first**

Complete the assertions begun in Task 3. In `tests/test_scripts.sh`:

- replace required `order_errors` and tombstone strings with
  `q3n no-frontier stripe progress passed`;
- assert the ABI headers do not contain `BLOCK_NEXT_PAGE` or
  `STAT_ORDER_ERRORS`;
- assert driver/QEMU sources do not contain `next_prog_page` or `tombstone`;
- stop requiring the invalid page-state/frontier fixture offsets.

- [ ] **Step 2: Run static smoke and verify RED**

Run: `./scripts/smoke-test.sh`

Expected: FAIL because the guest scripts and documentation still contain old
frontier/tombstone semantics.

- [ ] **Step 3: Replace the tombstone guest scenario**

In `rootfs/profile.d/mtd.sh`, replace the tombstone/frontier case with:

1. erase a block;
2. pause or fail the stripe-0 parity PROGRAM;
3. write logical pages 0..6;
4. write logical page 7, which maps to stripe-1 D0/physical page 8;
5. read logical page 7 back and compare it;
6. confirm stripe 0 remains unprotected;
7. confirm `parity_writes` did not increase for an unprotected placeholder;
8. print `q3n no-frontier stripe progress passed`.

Do not read an `order_errors` debugfs file.

- [ ] **Step 4: Reduce persistence smoke to supported behavior**

Keep fresh-image creation and raw data/OOB/BBM/overlay persistence checks.
Remove:

- tail parity replay stages;
- tail failure replay stages;
- persisted tombstone reason inspection;
- synthetic block-frontier/page-state corruption fixture;
- expectations that a new driver instance republishes prior PROTECTED state.

Document in both READMEs:

```text
QEMU persists raw NAND bytes and bitflip overlays.  The driver does not
restore Page-RAID runtime state after reload or VM restart in this phase.
```

- [ ] **Step 5: Run the complete verification matrix**

Run:

```bash
./scripts/smoke-test.sh
./scripts/build-qemu.sh
./scripts/build-kernel.sh
./scripts/build-rootfs.sh
./scripts/q3n-serial-smoke.sh
./scripts/q3n-persistence-smoke.sh
```

Expected:

- host/static tests pass;
- QEMU, kernel, and rootfs build;
- guest prints `q3n no-frontier stripe progress passed`;
- serial LDPC/MTD/RAID/BBM checks pass;
- persistence smoke validates raw media only;
- no log contains a program-order rejection.

- [ ] **Step 6: Record progress and commit**

Append to `.superpowers/sdd/progress.md`:

```text
Program-order boundary revision
Task 1: complete
Task 2: complete
Task 3: complete
Task 4: complete
Verification: host smoke, QEMU build, kernel build, rootfs build, serial guest,
raw-media persistence guest
```

Then commit:

```bash
git add rootfs/profile.d/mtd.sh rootfs/init \
  scripts/q3n-persistence-smoke.sh tests/test_scripts.sh \
  qemu/README.md README.md .superpowers/sdd/progress.md
git commit -m "test: verify q3n operation without program frontier"
```

---

## Final Review Checklist

- [ ] `rg -n "next_prog_page|program_state|program_order|serial_frontier|order_errors|tombstone|Q3N_MEDIA_PAGE_(ERASED|PRESENT|LOST)" qemu/hw qemu/include linux/drivers/mtd/nand/raw rootfs scripts` returns no matches.
- [ ] QEMU PROGRAM of an arbitrary or previously programmed page succeeds and stores bytewise AND.
- [ ] Erase clears main, OOB, and overlay without consulting page state.
- [ ] Next-stripe D0 completes while prior parity is queued or failed.
- [ ] RAID publishes only CRC-verified parity and never writes an unprotected placeholder.
- [ ] MTD LDPC corrected/`-EUCLEAN`/`-EBADMSG`, BBM, and OOB mappings retain their approved semantics.
- [ ] Restart recovery is described as out of scope rather than accidentally tested or implied.
