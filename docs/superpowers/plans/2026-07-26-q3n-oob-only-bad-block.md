# Q3N Independent OOB and Bad-Block Programming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Q3N logical OOB an independently readable/programable 128-byte controller space, place its BBM at physical page offset `0x4000`, and remove the dedicated mark-bad command so Linux marks blocks through OOB programming.

**Architecture:** A decoded physical page is fixed as `[16 KiB main][1 B OOB head/BBM][1536 B LDPC][127 B OOB tail]`. QEMU media exposes separate main+LDPC and logical-OOB operations; controller commands 6/7 transfer only 128-byte logical OOB. Linux sequences main and metadata programs explicitly and implements `_block_markbad` by programming logical OOB byte 0.

**Tech Stack:** QEMU C device model, QEMU BlockBackend, Linux MTD driver, KUnit/static shell tests, guest BusyBox smoke tests.

## Global Constraints

- `Q3N_PAGE_SIZE == 0x4000`.
- Decoded physical page size is `0x4680` bytes.
- Physical page offset `0x4000` is logical OOB byte 0 and the BBM.
- Physical page offsets `0x4001..0x4600` contain 1536 controller-owned LDPC bytes.
- Physical page offsets `0x4601..0x467f` contain logical OOB bytes 1..127.
- `READ_PAGE_OOB` and `PROGRAM_PAGE_OOB` transfer exactly 128 logical-OOB bytes and never transfer main data.
- OOB-only programming must preserve main and LDPC bytes exactly.
- Remove `Q3N_CMD_MARK_BAD_BLOCK` and every dedicated mark-bad helper from QEMU, media, Linux, tests, and documentation.
- Keep `Q3N_CMD_GET_BLOCK_STATUS`; block status remains BBM-only.
- Repeated programs use NAND `old & incoming`.
- QEMU and Linux must not add page-order/frontier state.
- Preserve the untracked `docs/qemu-3dnand-register-reference.md` until its documentation task explicitly stages it.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `qemu/include/hw/mtd/q3n-nand.h` | Shared QEMU layout constants, commands, status and MMIO ABI |
| `qemu/include/hw/mtd/q3n-media.h` | Persistent-media main/LDPC and logical-OOB interfaces |
| `qemu/hw/mtd/q3n-media.c` | Fixed page-slot construction and atomic NAND-AND updates |
| `qemu/hw/mtd/q3n-nand.c` | Logical-OOB mapping, LDPC generation/decoding and commands |
| `linux/drivers/mtd/nand/raw/qemu_3dnand.h` | Linux copy of controller ABI |
| `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c` | MMIO helpers, MTD data/OOB methods and `_block_markbad` |
| `tests/test_q3n_controller.c` | Executable layout/OOB mapping tests |
| `tests/test_q3n_overlay.c` | Physical-page offset and NAND merge tests |
| `tests/test_scripts.sh` | Cross-tree ABI and forbidden-symbol checks |
| `rootfs/profile.d/mtd.sh` | Guest OOB/BBM/data-preservation acceptance |
| `qemu/README.md` | Controller command and physical-page documentation |
| `docs/qemu-3dnand-register-reference.md` | Complete updated register reference |

---

### Task 1: Make the physical page layout explicit and add independent media OOB primitives

**Files:**
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `qemu/include/hw/mtd/q3n-media.h`
- Modify: `qemu/hw/mtd/q3n-media.c`
- Modify: `qemu/hw/mtd/q3n-media-overlay.h`
- Modify: `tests/test_q3n_overlay.c`
- Modify: `tests/test_q3n_overlay.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces:
  - `Q3N_PHYSICAL_OOB_HEAD_OFFSET`
  - `Q3N_PHYSICAL_OOB_HEAD_SIZE`
  - `Q3N_PHYSICAL_LDPC_OFFSET`
  - `Q3N_PHYSICAL_LDPC_SIZE`
  - `Q3N_PHYSICAL_OOB_TAIL_OFFSET`
  - `Q3N_PHYSICAL_OOB_TAIL_SIZE`
  - `Q3N_PHYSICAL_PAGE_SIZE`
  - `int q3n_media_read_logical_oob(Q3NMedia *, uint32_t block, uint32_t page, uint8_t logical_oob[Q3N_LOGICAL_OOB_SIZE])`
  - `int q3n_media_program_logical_oob(Q3NMedia *, uint32_t block, uint32_t page, const uint8_t logical_oob[Q3N_LOGICAL_OOB_SIZE])`
- Changes:
  - `q3n_media_read_page()` returns main, LDPC and overlays without returning guest OOB.
  - `q3n_media_program_page()` accepts main and generated LDPC, preserving OOB head/tail.

- [ ] **Step 1: Add failing static layout assertions**

Add to `tests/test_scripts.sh`:

```sh
header=qemu/include/hw/mtd/q3n-nand.h
assert_contains "$header" 'Q3N_PHYSICAL_OOB_HEAD_OFFSET[[:space:]]+Q3N_PAGE_SIZE'
assert_contains "$header" 'Q3N_PHYSICAL_OOB_HEAD_SIZE[[:space:]]+1U'
assert_contains "$header" 'Q3N_PHYSICAL_LDPC_OFFSET'
assert_contains "$header" 'Q3N_PHYSICAL_OOB_TAIL_OFFSET'
assert_contains "$header" 'Q3N_PHYSICAL_OOB_TAIL_SIZE'
assert_contains "$header" 'Q3N_PHYSICAL_PAGE_SIZE'
```

The Linux header receives and verifies the same constants in Task 3, when the
Linux ABI is updated.

Add forbidden legacy media-path assertions:

```sh
assert_not_contains qemu/include/hw/mtd/q3n-media.h 'q3n_media_mark_bad'
assert_not_contains qemu/hw/mtd/q3n-media.c 'q3n_media_mark_bad'
assert_not_contains qemu/hw/mtd/q3n-media.c 'q3n_media_write_bbm'
assert_contains qemu/include/hw/mtd/q3n-media.h 'q3n_media_read_logical_oob'
assert_contains qemu/include/hw/mtd/q3n-media.h 'q3n_media_program_logical_oob'
```

- [ ] **Step 2: Run the static test and verify RED**

Run:

```sh
./tests/test_scripts.sh
```

Expected: FAIL because the new constants and media OOB functions do not exist and the legacy mark-bad helpers still exist.

- [ ] **Step 3: Add a failing executable physical-layout test**

In `tests/test_q3n_overlay.c`, define:

```c
#define Q3N_PHYSICAL_OOB_HEAD_OFFSET  Q3N_PAGE_SIZE
#define Q3N_PHYSICAL_OOB_HEAD_SIZE    1U
#define Q3N_PHYSICAL_LDPC_OFFSET      (Q3N_PHYSICAL_OOB_HEAD_OFFSET + 1U)
#define Q3N_PHYSICAL_LDPC_SIZE        Q3N_LDPC_TOTAL_BYTES
#define Q3N_PHYSICAL_OOB_TAIL_OFFSET  \
        (Q3N_PHYSICAL_LDPC_OFFSET + Q3N_PHYSICAL_LDPC_SIZE)
#define Q3N_PHYSICAL_OOB_TAIL_SIZE    127U
#define Q3N_PHYSICAL_PAGE_SIZE        \
        (Q3N_PHYSICAL_OOB_TAIL_OFFSET + Q3N_PHYSICAL_OOB_TAIL_SIZE)
```

Add a test that starts with distinct main and LDPC sentinels, applies one logical-OOB program through a helper extracted from `q3n-media.c`, and asserts:

```c
assert(Q3N_PHYSICAL_OOB_HEAD_OFFSET == 0x4000U);
assert(Q3N_PHYSICAL_LDPC_OFFSET == 0x4001U);
assert(Q3N_PHYSICAL_OOB_TAIL_OFFSET == 0x4601U);
assert(Q3N_PHYSICAL_PAGE_SIZE == 0x4680U);
assert(page[0x4000] == logical_oob[0]);
assert(!memcmp(page + 0x4601, logical_oob + 1, 127));
assert_all_equal(page, 0, 0x4000, main_sentinel);
assert_all_equal(page, 0x4001, 0x4601, ldpc_sentinel);
```

Extract only the pure page-layout merge helpers into a generated test include in `tests/test_q3n_overlay.sh`, following the existing controller-helper extraction pattern.

- [ ] **Step 4: Run the executable test and verify RED**

Run:

```sh
./tests/test_q3n_overlay.sh
```

Expected: FAIL because the logical-OOB media merge helper is missing.

- [ ] **Step 5: Implement explicit layout constants and compile-time checks**

In `qemu/include/hw/mtd/q3n-nand.h` add:

```c
#define Q3N_PHYSICAL_OOB_HEAD_OFFSET  Q3N_PAGE_SIZE
#define Q3N_PHYSICAL_OOB_HEAD_SIZE    1U
#define Q3N_PHYSICAL_LDPC_OFFSET      \
    (Q3N_PHYSICAL_OOB_HEAD_OFFSET + Q3N_PHYSICAL_OOB_HEAD_SIZE)
#define Q3N_PHYSICAL_LDPC_SIZE        Q3N_LDPC_TOTAL_BYTES
#define Q3N_PHYSICAL_OOB_TAIL_OFFSET  \
    (Q3N_PHYSICAL_LDPC_OFFSET + Q3N_PHYSICAL_LDPC_SIZE)
#define Q3N_PHYSICAL_OOB_TAIL_SIZE    (Q3N_LOGICAL_OOB_SIZE - 1U)
#define Q3N_PHYSICAL_PAGE_SIZE        \
    (Q3N_PHYSICAL_OOB_TAIL_OFFSET + Q3N_PHYSICAL_OOB_TAIL_SIZE)
```

Replace the old physical-OOB arithmetic checks with checks for exact offsets
`0x4000`, `0x4001`, `0x4601` and size `0x4680`.

- [ ] **Step 6: Implement pure physical-page logical-OOB helpers**

Add pure helpers used by media I/O and tests:

```c
static void q3n_media_extract_logical_oob(const uint8_t *page,
                                           uint8_t *logical)
{
    logical[0] = page[Q3N_PHYSICAL_OOB_HEAD_OFFSET];
    memcpy(logical + 1, page + Q3N_PHYSICAL_OOB_TAIL_OFFSET,
           Q3N_PHYSICAL_OOB_TAIL_SIZE);
}

static void q3n_media_merge_logical_oob(uint8_t *page,
                                         const uint8_t *logical)
{
    q3n_media_merge_program(page + Q3N_PHYSICAL_OOB_HEAD_OFFSET,
                            logical, Q3N_PHYSICAL_OOB_HEAD_SIZE);
    q3n_media_merge_program(page + Q3N_PHYSICAL_OOB_TAIL_OFFSET,
                            logical + 1, Q3N_PHYSICAL_OOB_TAIL_SIZE);
}
```

These helpers must never touch main or LDPC ranges.

- [ ] **Step 7: Implement media OOB read/program**

`q3n_media_read_logical_oob()` reads and decodes one complete page slot, then
extracts head/tail.

`q3n_media_program_logical_oob()`:

```c
storage = g_malloc(m->page_stride);
ret = blk_pread(m->blk, slot, m->page_stride, storage, 0);
q3n_media_invert(storage, m->page_stride);
q3n_media_merge_logical_oob(storage, logical_oob);
q3n_media_invert(storage, m->page_stride);
ret = blk_pwrite(m->blk, slot, m->page_stride, storage, 0);
```

Permit a page-0 OOB program that changes a good BBM to non-`0xff`. Reject
ordinary programs to a block whose existing page-0 BBM is already bad, while
making a repeated identical mark-bad request return success.

Remove `q3n_media_write_bbm()` and `q3n_media_mark_bad()`.

- [ ] **Step 8: Split main+LDPC media I/O from logical OOB**

Change main page programming to merge only:

```c
q3n_media_merge_program(storage, data, Q3N_PAGE_SIZE);
q3n_media_merge_program(storage + Q3N_PHYSICAL_LDPC_OFFSET,
                        ldpc, Q3N_PHYSICAL_LDPC_SIZE);
```

It must preserve page offset `0x4000` and `0x4601..0x467f`.

Change main page reads to return main and the contiguous LDPC region needed by
the controller decoder.

- [ ] **Step 9: Run Task 1 tests and verify GREEN**

Run:

```sh
./tests/test_q3n_overlay.sh
./tests/test_scripts.sh
git diff --check
```

Expected: all pass.

- [ ] **Step 10: Commit Task 1**

```sh
git add \
  qemu/include/hw/mtd/q3n-nand.h \
  qemu/include/hw/mtd/q3n-media.h \
  qemu/hw/mtd/q3n-media.c \
  qemu/hw/mtd/q3n-media-overlay.h \
  tests/test_q3n_overlay.c \
  tests/test_q3n_overlay.sh \
  tests/test_scripts.sh
git commit -m "feat: split q3n physical oob around ldpc"
```

---

### Task 2: Convert QEMU commands 6/7 to OOB-only and remove MARK_BAD_BLOCK

**Files:**
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `tests/test_q3n_controller.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 1 media OOB primitives and fixed layout constants.
- Produces:
  - command 6 reads exactly 128 B logical OOB;
  - command 7 programs exactly 128 B logical OOB;
  - no command 9 mark-bad ABI;
  - main commands operate on main+LDPC only.

- [ ] **Step 1: Add failing command-contract assertions**

Add to `tests/test_scripts.sh`:

```sh
for file in \
  qemu/include/hw/mtd/q3n-nand.h \
  qemu/hw/mtd/q3n-nand.c; do
  assert_not_contains "$file" 'Q3N_CMD_MARK_BAD_BLOCK'
done
assert_not_contains qemu/hw/mtd/q3n-nand.c 'q3n_cmd_mark_bad_block'
assert_contains qemu/hw/mtd/q3n-nand.c \
  'data_count = Q3N_LOGICAL_OOB_SIZE'
```

The Linux command definition and helper are removed and verified in Task 3.

Add assertions rejecting any 16 KiB count in OOB command handlers.

- [ ] **Step 2: Run the static test and verify RED**

Run:

```sh
./tests/test_scripts.sh
```

Expected: FAIL on the legacy command and combined main+OOB transfer.

- [ ] **Step 3: Add controller mapping tests**

Extend `tests/test_q3n_controller.c` so a physical page has distinct main,
BBM, LDPC and tail values. Verify:

```c
q3n_physical_to_logical_oob(physical_page, logical);
assert(logical[0] == physical_page[0x4000]);
assert(!memcmp(logical + 1, physical_page + 0x4601, 127));

q3n_logical_to_physical_oob(logical, physical_page);
assert_main_unchanged();
assert_ldpc_unchanged();
```

The production change that makes this test pass is moving helper inputs from a
1664-byte OOB pointer to the complete decoded physical-page layout and using
the explicit head/tail offsets.

- [ ] **Step 4: Run controller test and verify RED**

Run:

```sh
./scripts/smoke-test.sh
```

Expected: FAIL in the controller helper test until mappings use the new page
offsets and mark-bad symbols are removed.

- [ ] **Step 5: Remove command 9**

Delete from QEMU:

```c
Q3N_CMD_MARK_BAD_BLOCK
q3n_cmd_mark_bad_block()
case Q3N_CMD_MARK_BAD_BLOCK:
```

Do not renumber commands 0..8. Command value 9 becomes unsupported and returns
the normal command error result.

- [ ] **Step 6: Implement OOB-only read command**

`q3n_cmd_read_page_oob()` must:

```c
validate_page_aligned_addr();
validate(s->oob_len == Q3N_LOGICAL_OOB_SIZE);
q3n_media_read_logical_oob(s->media, block, page, s->data_buf);
s->data_count = Q3N_LOGICAL_OOB_SIZE;
s->data_pos = 0;
q3n_finish_ok(s);
```

Do not call `q3n_clear_ecc_result()` or `q3n_decode_ldpc()`, and do not require
`LEN`. An OOB-only read leaves the latest main-page ECC result unchanged.

- [ ] **Step 7: Implement OOB-only program command**

`q3n_cmd_program_page_oob()` must validate `data_count >= 128`, then call only
`q3n_media_program_logical_oob()`. It must not call `q3n_generate_ldpc_step()`
and must not read any main bytes from the PIO buffer.

Increment `page_programs` and the matching foreground/parity-write counter only
after successful media programming.

- [ ] **Step 8: Restrict main commands to main+LDPC**

`q3n_cmd_program_page()` generates the 1536-byte LDPC buffer from the 16 KiB
main buffer and passes main+LDPC to Task 1's media function. It does not build
or merge logical OOB.

`q3n_cmd_read_page()` reads main+LDPC and uses the LDPC buffer for decode. It
does not read metadata or BBM as part of the main transfer.

- [ ] **Step 9: Run Task 2 tests and verify GREEN**

Run:

```sh
./scripts/smoke-test.sh
git diff --check
```

Expected: controller, overlay, static and host smoke tests pass.

- [ ] **Step 10: Commit Task 2**

```sh
git add \
  qemu/include/hw/mtd/q3n-nand.h \
  qemu/hw/mtd/q3n-nand.c \
  tests/test_q3n_controller.c \
  tests/test_scripts.sh
git commit -m "feat: make q3n oob commands independent"
```

---

### Task 3: Split Linux main/OOB I/O and mark bad through OOB programming

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 2 OOB-only command ABI.
- Produces:
  - `qemu_3dnand_read_phys_oob_locked()`
  - `qemu_3dnand_program_phys_oob_locked()`
  - `_block_markbad` implemented only through OOB PROGRAM.

- [ ] **Step 1: Add failing driver ABI tests**

Update `tests/test_scripts.sh`:

```sh
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h \
  'Q3N_CMD_MARK_BAD_BLOCK'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  'qemu_3dnand_mark_phys_block_bad_locked'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  'qemu_3dnand_read_phys_oob_locked'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  'qemu_3dnand_program_phys_oob_locked'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  'logical_oob\\[0\\] = 0x00'
```

Assert OOB helpers loop only over `Q3N_LOGICAL_OOB_SIZE`, not
`page_size + Q3N_LOGICAL_OOB_SIZE`.

- [ ] **Step 2: Run static tests and verify RED**

Run:

```sh
./tests/test_scripts.sh
```

Expected: FAIL because the combined helpers and dedicated mark-bad helper are
still present.

- [ ] **Step 3: Add KUnit tests for mark-bad buffer construction**

Extract a pure helper:

```c
static void qemu_3dnand_build_bad_block_oob(u8 *logical_oob)
{
    memset(logical_oob, 0xff, Q3N_LOGICAL_OOB_SIZE);
    logical_oob[0] = 0x00;
}
```

Add KUnit assertions:

```c
KUNIT_EXPECT_EQ(test, logical_oob[0], (u8)0x00);
for (i = 1; i < Q3N_LOGICAL_OOB_SIZE; i++)
    KUNIT_EXPECT_EQ(test, logical_oob[i], (u8)0xff);
```

- [ ] **Step 4: Run KUnit/static harness and verify RED**

Run:

```sh
./scripts/smoke-test.sh
```

Expected: FAIL until the helper is implemented and wired.

- [ ] **Step 5: Copy the physical layout ABI to Linux**

Add Task 1 constants and compile-time checks to
`linux/drivers/mtd/nand/raw/qemu_3dnand.h`. Remove
`Q3N_CMD_MARK_BAD_BLOCK`; keep command values 0..8 unchanged.

- [ ] **Step 6: Replace combined MMIO helpers**

Implement:

```c
static int qemu_3dnand_read_phys_oob_locked(
        struct qemu_3dnand *q3n, u32 block, u32 page,
        u8 logical_oob[Q3N_LOGICAL_OOB_SIZE], u32 op_class);

static int qemu_3dnand_program_phys_oob_locked(
        struct qemu_3dnand *q3n, u32 block, u32 page,
        const u8 logical_oob[Q3N_LOGICAL_OOB_SIZE], u32 op_class);
```

Both write `OOB_LEN=128`. The read helper consumes exactly 128 PIO bytes; the
program helper writes exactly 128 PIO bytes. Neither writes `LEN` nor accesses a
main buffer.

Keep separate main page helpers using commands 2/3 and exactly 16 KiB.

- [ ] **Step 7: Sequence main and metadata operations**

For a normal data or parity page program:

```c
ret = qemu_3dnand_program_phys_page_locked(..., data, op_class);
if (!ret)
    ret = qemu_3dnand_program_phys_oob_locked(..., logical_oob, op_class);
```

Only mark `data_page_valid` or a parity manifest valid after both commands
succeed.

For a data+OOB read, perform main read first, preserve its ECC result, then
perform the OOB-only read. For an OOB-only MTD request, skip main read entirely.

- [ ] **Step 8: Implement `_block_markbad` via OOB-only PROGRAM**

While holding the existing parity cancel barrier and `mtd_lock`:

```c
u8 logical_oob[Q3N_LOGICAL_OOB_SIZE];

qemu_3dnand_build_bad_block_oob(logical_oob);
ret = qemu_3dnand_program_phys_oob_locked(q3n, block, 0,
                                           logical_oob,
                                           Q3N_OP_FOREGROUND);
if (!ret)
    q3n->data_meta[block].bad = true;
```

Remove `qemu_3dnand_mark_phys_block_bad_locked()`. Keep repeated mark-bad
idempotent by returning 0 when runtime or physical block status is already bad.

- [ ] **Step 9: Correct partial-completion reporting**

For `_write_oob`:

- increment `retlen` only after main PROGRAM succeeds;
- increment `oobretlen` only after OOB PROGRAM succeeds;
- return the OOB error if main succeeded but OOB failed;
- do not set `data_page_valid` or queue parity until both succeed.

For `_read_oob`, OOB-only reads must not report main ECC bitflips.

- [ ] **Step 10: Run Task 3 tests and builds**

Run:

```sh
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-kernel.sh
git diff --check
```

Expected: all pass and the Q3N driver module builds.

- [ ] **Step 11: Commit Task 3**

```sh
git add \
  linux/drivers/mtd/nand/raw/qemu_3dnand.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c \
  tests/test_scripts.sh
git commit -m "feat: mark q3n bad blocks through oob"
```

---

### Task 4: Prove guest behavior, persistence and update documentation

**Files:**
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `scripts/q3n-persistence-smoke.sh`
- Modify: `tests/test_scripts.sh`
- Modify: `qemu/README.md`
- Modify: `README.md`
- Modify: `docs/qemu-3dnand-register-reference.md`

**Interfaces:**
- Consumes: Tasks 1-3 complete behavior.
- Produces: acceptance proof and user-facing documentation.

- [ ] **Step 1: Add failing guest acceptance**

Extend the OOB/BBM guest test:

1. erase a dedicated test block;
2. program deterministic main data into its first page;
3. read and save main data before mark-bad;
4. invoke the MTD mark-bad ioctl;
5. read OOB byte 0 and require `00`;
6. use an OOB-capable helper that can read the already-bad page without trying
   an ordinary MTD data write;
7. compare raw main data after mark-bad with the saved bytes;
8. require block-isbad;
9. require a second mark-bad call to succeed.

Add a helper mode if current `mtd_badblock` cannot perform the required
read-after-mark operation without violating MTD bad-block checks.

- [ ] **Step 2: Run guest smoke and verify RED**

Run:

```sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-serial-smoke.sh
```

Expected: FAIL until OOB-only command semantics and helper output are fully
wired into the rootfs.

- [ ] **Step 3: Add persistence acceptance**

In the prepare phase:

- program deterministic first-page main;
- mark the block through the MTD ioctl;
- save expected BBM and main digest.

In the verify phase:

- require BBM `00`;
- require block-isbad;
- require main digest unchanged from before mark-bad.

- [ ] **Step 4: Run persistence test and verify RED/GREEN**

Run before completing documentation:

```sh
./scripts/q3n-persistence-smoke.sh
```

Expected before final helper wiring: FAIL at the new assertion.

After wiring: PASS in both prepare and verify boots.

- [ ] **Step 5: Update QEMU and register documentation**

Document command 6/7 as 128-byte OOB-only transfers. Remove command 9. Replace
the physical-page description with:

```text
0x0000..0x3fff main
0x4000         OOB head / BBM / logical OOB[0]
0x4001..0x4600 LDPC
0x4601..0x467f OOB tail / logical OOB[1..127]
```

Update `docs/qemu-3dnand-register-reference.md` so:

- `PROGRAM_PAGE_OOB` no longer claims to transfer 16512 B;
- PIO OOB command length is 128 B;
- `MARK_BAD_BLOCK` is absent;
- `_block_markbad` is described as an OOB program.

- [ ] **Step 6: Run the full verification matrix**

Run fresh on the final tree:

```sh
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-serial-smoke.sh
./scripts/q3n-persistence-smoke.sh
git diff --check
```

Expected: every command exits 0. Guest logs must show OOB/BBM, serial RAID,
fault injection, parity failure continuation and persistence acceptance pass.

- [ ] **Step 7: Scan for removed ABI and stale semantics**

Run:

```sh
if rg -n \
  'Q3N_CMD_MARK_BAD_BLOCK|q3n_cmd_mark_bad_block|q3n_media_mark_bad|q3n_media_write_bbm|qemu_3dnand_mark_phys_block_bad_locked' \
  qemu linux rootfs scripts tests README.md \
  docs/qemu-3dnand-register-reference.md; then
    exit 1
fi
```

Historical design specifications and implementation plans are intentionally
excluded because they describe the removed ABI as background or a forbidden
symbol. Also require all current documented offsets and constants to match the
shared headers.

- [ ] **Step 8: Commit Task 4**

Stage only the acceptance and documentation files, including the pre-existing
untracked register reference:

```sh
git add \
  rootfs/profile.d/mtd.sh \
  scripts/q3n-persistence-smoke.sh \
  tests/test_scripts.sh \
  qemu/README.md \
  README.md \
  docs/qemu-3dnand-register-reference.md
git commit -m "test: verify q3n oob-only bad block marking"
```

---

## Final Review Checklist

- [ ] Physical page offsets are exactly `0x0000`, `0x4000`, `0x4001`,
  `0x4601`, and end at `0x4680`.
- [ ] QEMU command 6/7 transfer exactly 128 B.
- [ ] Main PROGRAM preserves OOB head/tail.
- [ ] OOB PROGRAM preserves main/LDPC.
- [ ] Dedicated mark-bad command and helpers have no remaining references.
- [ ] `_block_markbad` constructs logical OOB and uses OOB PROGRAM.
- [ ] BBM remains logical OOB byte 0.
- [ ] `GET_BLOCK_STATUS` remains BBM-only.
- [ ] Main/OOB partial completion is reported accurately.
- [ ] RAID metadata is considered valid only after both physical programs.
- [ ] All host, build, guest and persistence tests pass.
