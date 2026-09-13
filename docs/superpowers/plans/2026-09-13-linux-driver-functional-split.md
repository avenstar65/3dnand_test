# Linux Driver Functional Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the QEMU 3D NAND Linux driver by responsibility, add full READID-based YMTC device matching, and keep both multi-plane and compatibility builds behaviorally stable.

**Architecture:** Keep one shared `struct qemu_3dnand`, but move hardware, device, NAND-core, multi-plane profile, serial compatibility, and debugfs responsibilities into separate translation units with one-way dependencies. QEMU remains structurally unchanged; only its READID bytes change to the agreed YMTC ID.

**Tech Stack:** Linux 7.0.12 raw NAND/MTD APIs, PCI MMIO, KUnit, QEMU 11.0.2 C device model, POSIX shell, Python 3 unittest, Docker build wrappers.

**Spec:** `docs/superpowers/specs/2026-09-13-linux-driver-functional-split-design.md`

## Global Constraints

- Only Linux driver source organization is refactored; QEMU receives only the agreed READID value correction.
- Preserve MTD geometry, OOB, BBT, RAID mapping, error returns, debugfs names, and media format.
- Preserve `Q3N_ENABLE_MULTIPLANE_RAID=0|1`; only mode 1 registers `legacy.block_bad` and `legacy.block_markbad`.
- Ordinary functions created or moved by this refactor must contain at most 50 effective code lines, with no allow-list entries in this change.
- Keep `_locked` hardware helpers caller-locked and document the lock requirement at their declarations.
- Use full eight-byte ID matching; unknown and near-match IDs return `-ENODEV` and never register an MTD.
- Keep `qemu_3dnand.h` limited to the Linux/QEMU controller ABI and `qemu_3dnand_priv.h` limited to testable mapping/RAID/scheduler types.
- Do not add speculative vendor ops, manifest data, persistent RAID recovery state, or new serial RAID behavior.
- Use TDD for each behavior or architecture rule: add a failing test, observe the intended failure, make the minimum change, then rerun the focused and regression tests.

---

## Planned File Structure

**Create:**

- `linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h` — complete driver state, private metadata, lock annotations, and cross-module APIs.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_device.c` — private device table, full-ID matching, constraints, and runtime scan-ID construction.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_hw.c` — register access and physical NAND commands.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_profile.c` — multi-plane RAID1/RAID5 orchestration.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_serial.c` — compatibility D0..D6/P path and parity worker.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_nand.c` — NAND core callbacks, scan, BBT, and MTD registration.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_debugfs.c` — statistics and fault-injection debugfs interface.
- `tests/test_q3n_architecture.py` — module-boundary and function-size regression checks.

**Modify:**

- `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c` — reduce to module parameters and PCI lifecycle.
- `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c` — add exact/near/unknown device-ID tests.
- `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand` — link the new objects and propagate the mode macro.
- `scripts/apply-linux-overlay.sh` — copy every new driver file atomically.
- `qemu/hw/mtd/q3n-nand.c` — return the agreed YMTC ID.
- `tests/test_q3n_controller.c` and `tests/test_scripts.sh` — test READID and new file/build wiring.
- `README.md`, `qemu/README.md`, and the architecture/design documents — describe the resulting layout and real READID path.

Existing `qemu_3dnand_map.c`, `qemu_3dnand_mp.c`, `qemu_3dnand_raid.c`, and
`qemu_3dnand_sched.c` retain their algorithmic responsibilities.

---

### Task 1: Add Architecture and Function-Size Test Infrastructure

**Files:**
- Create: `tests/test_q3n_architecture.py`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: repository-relative Linux driver paths.
- Produces: `function_spans(path) -> list[tuple[str, int, int, int]]` and architecture tests runnable with `python3 tests/test_q3n_architecture.py -v`.

- [ ] **Step 1: Record the pre-refactor verification baseline**

Run the current static tests, both compile modes, and required multi-plane runtime tests before moving code:

```bash
python3 tests/test_nand_core_patches.py -v
sh tests/test_q3n_overlay.sh
sh tests/test_scripts.sh
Q3N_ENABLE_MULTIPLANE_RAID=0 ./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-serial-smoke.sh
Q3N_ENABLE_MULTIPLANE_RAID=1 ./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-kunit-smoke.sh
./scripts/q3n-raid1-smoke.sh
./scripts/q3n-raid5-smoke.sh
./scripts/q3n-persistence-smoke.sh
```

Record whether the serial smoke passes. Only a passing pre-refactor result becomes a final gate because new serial behavior is out of scope.

- [ ] **Step 2: Write tests for the source parser using literal C fixtures**

Create temporary fixtures inside Python tests. One 50-line function must pass and one 51-line function must fail. Count nonblank, non-comment lines between the outer braces; do not count a multiline signature.

```python
def test_function_limit_accepts_50_effective_lines(self):
    body = "\n".join(f"\tv += {i};" for i in range(49))
    source = f"static int q3n_ok(int v)\n{{\n{body}\n\treturn v;\n}}\n"
    self.assertEqual(find_oversized_functions(source, 50), [])

def test_function_limit_rejects_51_effective_lines(self):
    body = "\n".join(f"\tv += {i};" for i in range(50))
    source = f"static int q3n_too_long(int v)\n{{\n{body}\n\treturn v;\n}}\n"
    self.assertEqual(find_oversized_functions(source, 50)[0][0],
                     "q3n_too_long")
```

- [ ] **Step 3: Run the focused test and verify RED**

Run: `python3 tests/test_q3n_architecture.py -v`

Expected: import/name failure for `find_oversized_functions`, proving the fixture exercises the missing parser.

- [ ] **Step 4: Implement the minimal comment-aware function scanner**

Implement a small lexical scanner that removes block comments, line comments, string literals, and character literals before brace counting. Detect top-level function bodies whose declaration ends in `)` and whose next token is `{`; exclude control statements and macro definitions.

```python
def find_oversized_functions(source: str, limit: int):
    clean = strip_comments_and_literals(source)
    spans = function_spans(clean)
    return [span for span in spans if span[3] > limit]
```

The test output must include function name, start/end line, and effective line count.

- [ ] **Step 5: Add architecture assertions without enabling the final file list yet**

Add reusable assertions for file existence, forbidden symbol ownership, include dependencies, and a cycle-free allowed dependency map. Test these helpers against temporary fixture files so Task 1 is green before production files exist.

```python
ALLOWED_DEPENDENCIES = {
    "main": {"internal", "device", "hw", "nand", "debugfs"},
    "nand": {"internal", "hw", "profile", "serial"},
    "profile": {"internal", "hw", "map", "mp", "raid"},
    "serial": {"internal", "hw", "map", "raid", "sched"},
    "debugfs": {"internal", "hw"},
    "device": {"internal"},
    "hw": {"internal"},
}
```

- [ ] **Step 6: Run focused and shell regression tests**

Run:

```bash
python3 tests/test_q3n_architecture.py -v
sh tests/test_scripts.sh
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add tests/test_q3n_architecture.py tests/test_scripts.sh
git commit -m "test: add q3n architecture guards"
```

---

### Task 2: Extract Shared State and Physical Hardware Access

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_hw.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c:25-382`
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `tests/test_q3n_architecture.py`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces:

```c
u32 q3n_hw_readl(struct qemu_3dnand *q3n, u32 reg);
void q3n_hw_writel(struct qemu_3dnand *q3n, u32 reg, u32 value);
int q3n_hw_wait_ready_locked(struct qemu_3dnand *q3n);
int q3n_hw_read_id_locked(struct qemu_3dnand *q3n, u8 *id, size_t len);
int q3n_hw_read_page_locked(struct qemu_3dnand *q3n, u32 block, u32 page,
                            u8 *buf, u32 op_class,
                            struct q3n_ecc_result *ecc);
int q3n_hw_read_oob_locked(struct qemu_3dnand *q3n, u32 block, u32 page,
                           u8 oob[Q3N_LOGICAL_OOB_SIZE], u32 op_class);
int q3n_hw_program_page_locked(struct qemu_3dnand *q3n, u32 block, u32 page,
                               const u8 *buf, u32 op_class);
int q3n_hw_program_oob_locked(struct qemu_3dnand *q3n, u32 block, u32 page,
                              const u8 oob[Q3N_LOGICAL_OOB_SIZE],
                              u32 op_class);
int q3n_hw_erase_block_locked(struct qemu_3dnand *q3n, u32 block);
int q3n_hw_get_block_status_locked(struct qemu_3dnand *q3n, u32 block,
                                   u32 *status);
```

- [ ] **Step 1: Add failing ownership tests**

Require `internal.h` and `hw.c`, require the hardware symbols in `hw.c`, and forbid their old `qemu_3dnand_*` definitions in `main.c`.

- [ ] **Step 2: Run RED**

Run: `python3 tests/test_q3n_architecture.py -v`

Expected: failure reporting missing `qemu_3dnand_internal.h` and `qemu_3dnand_hw.c`.

- [ ] **Step 3: Move shared structs and constants into the internal header**

Move `qemu_3dnand_data_block_meta`, `qemu_3dnand_parity_entry`, `qemu_3dnand`, and `qemu_3dnand_parity_work`. Add `q3n->raid_level`, `q3n->nand_id[8]`, `q3n->nand_id_len`, and an opaque `const struct q3n_device_desc *device`. Group fields by lifecycle, NAND, hardware geometry, profile, serial parity, and statistics.

- [ ] **Step 4: Move and rename hardware helpers**

Move register access, address calculation, ECC result loading, physical page/OOB program/read, erase, and block-status functions to `hw.c`. Preserve register order and exact errno behavior. Add `lockdep_assert_held(&q3n->mtd_lock)` to every `_locked` entry point.

- [ ] **Step 5: Wire build and overlay copying**

Add `qemu_3dnand_hw.o` to `qemu_3dnand-y`; add `qemu_3dnand_internal.h` and `qemu_3dnand_hw.c` to the atomic overlay file list. Propagate `Q3N_ENABLE_MULTIPLANE_RAID` to each new mode-aware object through per-object `CFLAGS_*.o` assignments, not directory-wide `ccflags-y`.

- [ ] **Step 6: Run focused regression tests and both compile modes**

```bash
python3 tests/test_q3n_architecture.py -v
sh tests/test_scripts.sh
Q3N_ENABLE_MULTIPLANE_RAID=0 ./scripts/shell.sh ./scripts/build-kernel.sh
Q3N_ENABLE_MULTIPLANE_RAID=1 ./scripts/shell.sh ./scripts/build-kernel.sh
```

Expected: all pass; no new lockdep or unused-function build error.

- [ ] **Step 7: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_hw.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
  scripts/apply-linux-overlay.sh tests/test_q3n_architecture.py tests/test_scripts.sh
git commit -m "refactor: extract q3n hardware access"
```

---

### Task 3: Add Device Table and Real READID Matching

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_device.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `qemu/hw/mtd/q3n-nand.c:480-488`
- Modify: `tests/test_q3n_controller.c`
- Modify: `tests/test_q3n_architecture.py`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: `q3n_hw_read_id_locked()` and geometry/capability fields already stored in `struct qemu_3dnand`.
- Produces:

```c
const struct q3n_device_desc *q3n_device_match(const u8 *id, size_t len);
int q3n_device_validate(struct qemu_3dnand *q3n,
                        const struct q3n_device_desc *device);
int q3n_device_build_scan_id(struct qemu_3dnand *q3n,
                             struct nand_flash_dev *scan_id,
                             u32 writesize, u32 oobsize,
                             u32 erasesize, u64 size);
const char *q3n_device_name(const struct q3n_device_desc *device);
```

- [ ] **Step 1: Write failing KUnit device-match tests**

```c
static void q3n_device_full_id_match_test(struct kunit *test)
{
    static const u8 exact[] = { 0x9c, 0xd7, 0x98, 0xa6,
                                0x51, 0x33, 0x4e, 0x44 };
    u8 near[sizeof(exact)];

    memcpy(near, exact, sizeof(near));
    near[7] ^= 1;
    KUNIT_EXPECT_NOT_NULL(test, q3n_device_match(exact, sizeof(exact)));
    KUNIT_EXPECT_PTR_EQ(test, q3n_device_match(near, sizeof(near)), NULL);
    KUNIT_EXPECT_PTR_EQ(test, q3n_device_match(exact, 1), NULL);
}
```

Also add validation cases for a missing capability and mismatched ECC strength, each expecting `-EINVAL`.

- [ ] **Step 2: Write a failing QEMU READID test**

Move the default ID constant inside `Q3N_CONTROLLER_HELPERS_BEGIN/END`, include it in the existing host controller test, and assert exact equality with the eight agreed bytes. Before changing QEMU, `sh tests/test_scripts.sh` must fail because byte 0 is `0x2c`.

- [ ] **Step 3: Run RED**

```bash
sh tests/test_scripts.sh
Q3N_ENABLE_MULTIPLANE_RAID=1 ./scripts/shell.sh ./scripts/build-kernel.sh
```

Expected: READID mismatch and undefined `q3n_device_match` failures.

- [ ] **Step 4: Implement the private descriptor table**

Define one private descriptor named `YMTC QEMU 3D NAND` with exact ID
`9c d7 98 a6 51 33 4e 44`, required `Q3N_CAP_BASIC_FLASH |
Q3N_CAP_PERSISTENT_MEDIA | Q3N_CAP_BAD_BLOCK_MARKER`, two dies, four planes,
16 KiB pages, 128-byte logical OOB, 1664-byte physical OOB, 1600 pages/block,
1024/40 ECC, and 96-byte × 16-step LDPC. GEOM0 validates logical OOB while
physical OOB validates the compiled Linux/QEMU media ABI constants. Match the
descriptor's full declared ID length.

- [ ] **Step 5: Replace Linux's synthetic READID**

During probe, lock `mtd_lock`, call `q3n_hw_read_id_locked`, store the bytes,
match and validate the descriptor, and return `-ENODEV` for unknown IDs.
`cmdfunc(NAND_CMD_READID)` must call `q3n_hw_read_id_locked` and populate the
legacy read buffer with the real result; delete the static ID array from the
NAND callback path.

- [ ] **Step 6: Correct QEMU's READID bytes**

```c
static const uint8_t q3n_default_nand_id[] = {
    0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44,
};
```

Make `q3n_cmd_read_id()` copy this constant without changing command status or data-window behavior.

- [ ] **Step 7: Wire the device object into driver and KUnit builds**

Add `qemu_3dnand_device.o` to both `qemu_3dnand-y` and `qemu_3dnand_test-y`, and add the file to the overlay script and architecture checks.

- [ ] **Step 8: Run GREEN and integration tests**

```bash
sh tests/test_scripts.sh
python3 tests/test_q3n_architecture.py -v
./scripts/shell.sh ./scripts/build-qemu.sh
Q3N_ENABLE_MULTIPLANE_RAID=1 ./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-kunit-smoke.sh
```

Expected: exact ID test, device KUnit test, QEMU build, Linux build, and KUnit smoke all pass; boot logs name the matched YMTC descriptor.

- [ ] **Step 9: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_device.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c \
  linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
  scripts/apply-linux-overlay.sh qemu/hw/mtd/q3n-nand.c \
  tests/test_q3n_controller.c tests/test_q3n_architecture.py tests/test_scripts.sh
git commit -m "feat: identify q3n NAND devices by full ID"
```

---

### Task 4: Extract Multi-plane RAID Profile Orchestration

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_profile.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c:780-1054,1543-1657`
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `tests/test_q3n_architecture.py`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: `q3n_map_raid1_page`, `q3n_map_raid5_stripe`, `q3n_mp_*`, `q3n_raid5_recover`, and caller-held `mtd_lock`.
- Produces:

```c
int q3n_profile_read_page_locked(struct qemu_3dnand *q3n, int page, u8 *buf);
int q3n_profile_write_page_locked(struct qemu_3dnand *q3n, int page,
                                  const u8 *buf);
int q3n_profile_read_oob_locked(struct qemu_3dnand *q3n, int page, u8 *oob);
int q3n_profile_write_oob_locked(struct qemu_3dnand *q3n, int page,
                                 const u8 *oob, bool *marker_written);
int q3n_profile_erase_page_locked(struct qemu_3dnand *q3n, u32 page);
int q3n_profile_block_bad(struct qemu_3dnand *q3n, loff_t ofs);
int q3n_profile_block_markbad(struct qemu_3dnand *q3n, loff_t ofs);
```

- [ ] **Step 1: Add failing ownership and size tests**

Require all `q3n_profile_*` orchestration definitions in `profile.c`, forbid them in `main.c`, and enable the 50-line gate for `profile.c`.

- [ ] **Step 2: Run RED**

Run: `python3 tests/test_q3n_architecture.py -v`

Expected: missing `profile.c` and old symbol ownership failures.

- [ ] **Step 3: Move mapping/buffer preparation and write orchestration**

Split write preparation into RAID1 mirror-buffer and RAID5 data/parity-buffer helpers. Keep `q3n_recovery_begin/complete`, member masks, parity rotation, and partial-failure returns unchanged.

- [ ] **Step 4: Split the long read recovery function**

Use separate helpers for transport-result validation, RAID1 copy/fallback, RAID5 data collection, and RAID5 one-member recovery. Preserve ECC corrected/failed accounting and require RAM recovery eligibility exactly where it is required today.

- [ ] **Step 5: Move profile OOB, erase, and block operations**

Keep whole-LEB recovery invalidation before OOB program and erase. Preserve full member-mask success requirements and multi-plane-only bad-block broadcast behavior.

- [ ] **Step 6: Wire and run GREEN**

```bash
python3 tests/test_q3n_architecture.py -v
sh tests/test_scripts.sh
Q3N_ENABLE_MULTIPLANE_RAID=1 ./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-raid1-smoke.sh
./scripts/q3n-raid5-smoke.sh
```

Expected: profile functions satisfy 50 lines; both RAID levels retain recovery and double-failure behavior.

- [ ] **Step 7: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_profile.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
  scripts/apply-linux-overlay.sh tests/test_q3n_architecture.py tests/test_scripts.sh
git commit -m "refactor: extract multi-plane RAID profile"
```

---

### Task 5: Extract Serial Compatibility and Parity Worker

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_serial.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c:135-779,1055-1578`
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `tests/test_q3n_architecture.py`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: physical `q3n_hw_*_locked` operations, serial mapping, RAID XOR/rebuild, and scheduler APIs.
- Produces:

```c
int q3n_serial_read_page_locked(struct qemu_3dnand *q3n, int page, u8 *buf);
int q3n_serial_write_page(struct qemu_3dnand *q3n, int page,
                          const u8 *buf);
int q3n_serial_read_oob_locked(struct qemu_3dnand *q3n, int page, u8 *oob);
int q3n_serial_write_oob_locked(struct qemu_3dnand *q3n, int page,
                                const u8 *oob, bool *marker_written);
int q3n_serial_erase_page_locked(struct qemu_3dnand *q3n, u32 page);
void q3n_serial_sync(struct qemu_3dnand *q3n);
```

- [ ] **Step 1: Add failing ownership and size tests**

Require serial parity worker, generation tracking, queue/cancel, and compatibility MTD helpers in `serial.c`; forbid those definitions in `main.c`; enable the 50-line gate for `serial.c`.

- [ ] **Step 2: Run RED**

Run: `python3 tests/test_q3n_architecture.py -v`

Expected: missing `serial.c` and old symbol ownership failures.

- [ ] **Step 3: Move simple serial mapping and metadata helpers**

Move logical decode, data/parity indices, generation checks, parity stale accounting, and serial read/write/OOB/erase helpers without changing calculations.

- [ ] **Step 4: Decompose the parity worker into named phases**

Create helpers for pause handling, scheduler acquisition, member read/rebuild, parity commit, and common completion. Every failure path must still release reservations, cancel/requeue requests correctly, wake block waiters, and free all three allocations.

- [ ] **Step 5: Preserve lifecycle boundaries**

`q3n_serial_sync()` must flush the workqueue before scheduler drain. Top-level lifecycle code continues to destroy the workqueue and free metadata only after MTD unregister and NAND cleanup.

- [ ] **Step 6: Run GREEN in both modes**

```bash
python3 tests/test_q3n_architecture.py -v
sh tests/test_scripts.sh
Q3N_ENABLE_MULTIPLANE_RAID=0 ./scripts/shell.sh ./scripts/build-kernel.sh
Q3N_ENABLE_MULTIPLANE_RAID=1 ./scripts/shell.sh ./scripts/build-kernel.sh
```

If `./scripts/q3n-serial-smoke.sh` passes at the recorded pre-refactor baseline, run it here and require it to remain green. Do not add new serial functionality to make a pre-existing baseline failure pass.

- [ ] **Step 7: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_serial.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
  scripts/apply-linux-overlay.sh tests/test_q3n_architecture.py tests/test_scripts.sh
git commit -m "refactor: extract serial RAID compatibility"
```

---

### Task 6: Extract NAND Core and ECC Adapter

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_nand.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c:1660-2167`
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `tests/test_q3n_architecture.py`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: device scan-ID construction, real hardware READID, profile façade, serial façade, and shared lifecycle state.
- Produces:

```c
int q3n_nand_register(struct qemu_3dnand *q3n);
void q3n_nand_unregister(struct qemu_3dnand *q3n);
void q3n_nand_cleanup(struct qemu_3dnand *q3n);
```

- [ ] **Step 1: Add failing NAND ownership tests**

Require `nand_scan_with_ids`, `nand_chip.ecc.*`, legacy command callbacks, OOB layout, and mode-gated bad-block callback registration in `nand.c`; forbid them in `main.c`; enable the 50-line gate for `nand.c`.

- [ ] **Step 2: Run RED**

Run: `python3 tests/test_q3n_architecture.py -v`

Expected: missing `nand.c` and old ownership failures.

- [ ] **Step 3: Move OOB layout and ECC callbacks**

Keep NAND callbacks as thin mode dispatchers. Each callback acquires/releases `mtd_lock` at the same boundary and calls either `q3n_profile_*` or `q3n_serial_*`. Preserve raw-main `-EOPNOTSUPP` and raw-OOB BBM behavior.

- [ ] **Step 4: Move legacy command and status callbacks**

Keep ERASE1 address validation and ERASE2 execution semantics. READID must issue the hardware command, STATUS must report prior legacy error, RESET must clear pending erase, and `waitfunc` must propagate negative errno.

- [ ] **Step 5: Move attach, scan, BBT, and MTD registration**

Build one runtime scan ID from the matched descriptor, set callbacks before `nand_scan_with_ids`, register MTD only after successful BBT creation, and clean up NAND immediately on post-scan geometry or registration failure.

- [ ] **Step 6: Verify callback gating after preprocessing and compilation**

```bash
sh tests/test_scripts.sh
Q3N_ENABLE_MULTIPLANE_RAID=0 ./scripts/shell.sh ./scripts/build-kernel.sh
nm -a work/build/linux-7.0.12/drivers/mtd/nand/raw/qemu_3dnand_nand.o | \
  grep -E 'q3n_nand_block_(bad|markbad)$' && exit 1 || true
Q3N_ENABLE_MULTIPLANE_RAID=1 ./scripts/shell.sh ./scripts/build-kernel.sh
nm -a work/build/linux-7.0.12/drivers/mtd/nand/raw/qemu_3dnand_nand.o | \
  grep -E 'q3n_nand_block_(bad|markbad)$'
```

Expected: mode 0 build has neither private symbol; mode 1 has both.

- [ ] **Step 7: Run KUnit and MTD smoke tests**

```bash
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-kunit-smoke.sh
./scripts/q3n-raid1-smoke.sh
./scripts/q3n-raid5-smoke.sh
```

Expected: all pass, including BBT scan and markbad behavior.

- [ ] **Step 8: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_nand.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
  scripts/apply-linux-overlay.sh tests/test_q3n_architecture.py tests/test_scripts.sh
git commit -m "refactor: extract NAND core adapter"
```

---

### Task 7: Extract Debugfs and Reduce Main to PCI Lifecycle

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_debugfs.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c:2168-2975`
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `tests/test_q3n_architecture.py`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: shared statistics, hardware fault helpers, serial scheduler counters, and lifecycle-ready device state.
- Produces:

```c
void q3n_debugfs_init(struct qemu_3dnand *q3n);
void q3n_debugfs_remove(struct qemu_3dnand *q3n);
void q3n_debugfs_unpause(struct qemu_3dnand *q3n);
```

- [ ] **Step 1: Add failing debugfs/main ownership tests**

Require all `DEFINE_DEBUGFS_ATTRIBUTE`, stat getters, fault setters, and `debugfs_create_file` calls in `debugfs.c`. Require `main.c` to contain only module parameters, PCI lifecycle, PCI ID table, and module metadata. Enable the 50-line gate for `debugfs.c` and `main.c`.

- [ ] **Step 2: Run RED**

Run: `python3 tests/test_q3n_architecture.py -v`

Expected: missing `debugfs.c`, forbidden debugfs symbols in `main.c`, and oversized `probe`.

- [ ] **Step 3: Move debugfs in functional groups**

Split registration into hardware statistics, RAID statistics, scheduler state, parity pause controls, and fault injection helpers. Keep every existing filename and mode unchanged.

- [ ] **Step 4: Decompose probe without changing order**

Use the following top-level shape, with every helper below 50 effective lines:

```c
static int q3n_probe(struct pci_dev *pdev,
                     const struct pci_device_id *id)
{
    struct qemu_3dnand *q3n;
    int ret;

    ret = q3n_pci_prepare(pdev, &q3n);
    if (ret)
        return ret;
    ret = q3n_discover_device(q3n);
    if (ret)
        return q3n_probe_failed(q3n, ret);
    ret = q3n_alloc_runtime(q3n);
    if (ret)
        return q3n_probe_failed(q3n, ret);
    ret = q3n_nand_register(q3n);
    if (ret)
        return q3n_probe_failed(q3n, ret);
    q3n_debugfs_init(q3n);
    q3n_log_geometry(q3n);
    return 0;
}
```

The implementation must preserve the existing allocation-before-scan requirement and reverse-order rollback.

- [ ] **Step 5: Decompose remove into explicit reverse lifecycle calls**

Call `q3n_debugfs_remove`, `q3n_debugfs_unpause`, `q3n_nand_unregister`, workqueue flush, `q3n_nand_cleanup`, workqueue destruction, and metadata release in the same order as the pre-refactor driver.

- [ ] **Step 6: Enable the final architecture and function-size file list**

Run the 50-line check over exactly:

```text
qemu_3dnand_main.c
qemu_3dnand_device.c
qemu_3dnand_hw.c
qemu_3dnand_nand.c
qemu_3dnand_profile.c
qemu_3dnand_serial.c
qemu_3dnand_debugfs.c
```

Assert dependency edges against `ALLOWED_DEPENDENCIES` and require no strongly connected component larger than one module.

- [ ] **Step 7: Run GREEN and builds**

```bash
python3 tests/test_q3n_architecture.py -v
sh tests/test_scripts.sh
Q3N_ENABLE_MULTIPLANE_RAID=0 ./scripts/shell.sh ./scripts/build-kernel.sh
Q3N_ENABLE_MULTIPLANE_RAID=1 ./scripts/shell.sh ./scripts/build-kernel.sh
```

Expected: architecture checks, function-size checks, and both builds pass without warnings treated as errors.

- [ ] **Step 8: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_debugfs.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
  scripts/apply-linux-overlay.sh tests/test_q3n_architecture.py tests/test_scripts.sh
git commit -m "refactor: isolate q3n PCI lifecycle"
```

---

### Task 8: Full Compatibility Verification and Documentation

**Files:**
- Modify: `README.md`
- Modify: `qemu/README.md`
- Modify: `docs/superpowers/specs/2026-09-13-linux-driver-functional-split-design.md`
- Modify: `docs/superpowers/specs/2026-08-30-multiplane-page-raid1-raid5-design.md`
- Modify: `docs/superpowers/plans/2026-09-13-linux-driver-functional-split.md`

**Interfaces:**
- Consumes: all completed driver modules and test commands.
- Produces: documented final architecture, verified behavior matrix, and a clean branch ready for review/push.

- [ ] **Step 1: Update user-facing architecture and device documentation**

Document every new file, the real READID path, the YMTC ID, full-ID rejection behavior, macro ownership, and the 50-line rule. Update the design status only after all checks pass.

- [ ] **Step 2: Run all static and host tests**

```bash
python3 tests/test_q3n_architecture.py -v
python3 tests/test_nand_core_patches.py -v
sh tests/test_q3n_overlay.sh
sh tests/test_scripts.sh
git diff --check
```

Expected: zero failures and no whitespace errors.

- [ ] **Step 3: Rebuild QEMU and mode 0 Linux**

```bash
./scripts/shell.sh ./scripts/build-qemu.sh
Q3N_ENABLE_MULTIPLANE_RAID=0 ./scripts/shell.sh ./scripts/build-kernel.sh
```

Expected: both builds succeed. Inspect `qemu_3dnand_nand.o` and require no private multi-plane bad-block callback symbols.

- [ ] **Step 4: Rebuild mode 1 Linux and rootfs**

```bash
Q3N_ENABLE_MULTIPLANE_RAID=1 ./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
```

Expected: kernel, modules, and initramfs succeed; mode 1 object contains both private bad-block callback symbols.

- [ ] **Step 5: Run all multi-plane acceptance tests**

```bash
./scripts/q3n-kunit-smoke.sh
./scripts/q3n-raid1-smoke.sh
./scripts/q3n-raid5-smoke.sh
./scripts/q3n-persistence-smoke.sh
```

Expected: every host wrapper reports its acceptance marker; RAID5 still recovers one failed member and reports the expected error for two failed members.

- [ ] **Step 6: Compare runtime geometry and interfaces with the recorded baseline**

Check boot output/debugfs for MTD size, writesize, erasesize, oobsize, matched device name, BBT result, and the existing debugfs filenames. Only the READID/name change may differ.

- [ ] **Step 7: Mark plan and design verification results**

Record the exact commands and outcomes in the design's implementation-result section and check completed plan boxes. Do not claim serial runtime support beyond the pre-refactor baseline.

- [ ] **Step 8: Commit final documentation and verification metadata**

```bash
git add README.md qemu/README.md \
  docs/superpowers/specs/2026-09-13-linux-driver-functional-split-design.md \
  docs/superpowers/specs/2026-08-30-multiplane-page-raid1-raid5-design.md \
  docs/superpowers/plans/2026-09-13-linux-driver-functional-split.md
git commit -m "docs: record q3n driver split verification"
```

- [ ] **Step 9: Review and push only after verification**

Run `git status --short`, inspect the full branch diff from the design commit, request a code review, fix any Critical/Important findings, rerun affected checks, then push `codex/multiplane-page-raid1-raid5`. Leave `.vscode/` untracked.
