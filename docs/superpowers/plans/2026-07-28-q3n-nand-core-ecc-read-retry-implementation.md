# Q3N NAND Core ECC and Read-Retry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the direct-MTD/page-RAID Q3N driver with an extensible raw-NAND controller driver initialized by `nand_scan_with_ids()`, using NAND Core ECC, bad-block, BBT, and read-retry paths while preserving the device's 16 KiB page and 1600-page non-power-of-two eraseblock geometry through tracked Linux patches.

**Architecture:** A PCI/module entry owns one controller and one NAND chip. Device detection reads the complete eight-byte ID, selects an independent flash profile from `ytmc_nand.c`, and passes that whitelist table to `nand_scan_with_ids()`. NAND Core owns MTD operations, bad-block callbacks, RAM BBT, and retry iteration; the driver owns `exec_op`, ECC page/OOB/raw callbacks, retry-mode programming, address conversion, and register access. The QEMU model exposes the matching ID, raw reads, retry mode, and deterministic bit-error behavior. Linux source compatibility changes remain versioned patch files and are never copied into this repository as modified upstream source.

**Tech Stack:** Linux 7.0.12 raw NAND/MTD APIs, C11 host behavior tests, KUnit, QEMU 11.0.2 device model, POSIX shell overlay/build scripts, `git apply`.

## Execution Status

| Tasks | Status | Evidence |
| --- | --- | --- |
| 1–4 Linux exact-geometry patch pipeline | Complete | Three ordered, idempotent patches; arithmetic and BBT boundary tests |
| 5–12 driver/model implementation | Complete | Eight-object KO; exact ID whitelist; controller/ECC/raw/read-retry host tests |
| 13 NAND Core BBT ownership | Complete | Compiled-symbol contract plus guest markbad/BBT rescan |
| 14 Linux/QEMU builds | Complete | Linux 7.0.12 KO/bzImage and QEMU 11.0.2 x86_64 build |
| 15 guest integration | Partial | Probe, geometry, read/write/erase, OOB/BBM and persistence pass; guest read-retry fault injection is not exposed |
| 16 documentation/GitHub | In progress | Documentation reconciled; final fresh verification and PR update remain |

The unchecked boxes below preserve the original RED/GREEN implementation recipe; this
table records the actual execution outcome after implementation.

## Global Constraints

- Work only on `codex/nand-core-ecc-read-retry` in the isolated worktree.
- Do not edit a checked-out Linux upstream tree. Produce and validate patches under `linux/patches/`.
- Do not assign `mtd->_read`, `mtd->_write`, `mtd->_erase`, `mtd->_read_oob`, `mtd->_write_oob`, `mtd->_block_isbad`, or `mtd->_block_markbad` in Q3N code.
- Do not set `NAND_SKIP_BBTSCAN`; do not allocate or maintain a private BBT.
- Do not link page RAID or the legacy scheduler into the production module.
- Register access (`readl`, `writel`, BAR offsets) is restricted to `qemu_3dnand_hw.c`.
- Support exactly one target and one LUN in this phase; reject unsupported target/LUN positions.
- Use full-ID matching for `9c d7 98 a6 51 33 4e 44`.
- Read-retry modes are 0, 1, 2, 3 with gains 0, 8, 16, 24. Mode zero is restored after a page read and after reset.
- An uncorrectable ECC attempt increments `mtd->ecc_stats.failed` and returns a non-negative bitflip value so NAND Core can advance retry mode. Transport/controller failures remain negative errors.
- Every task uses RED → GREEN → REFACTOR. A test must exercise behavior, not merely grep implementation text.

---

## Task 1: Add a strict, idempotent Linux patch application pipeline

**Files:**
- Create: `linux/patches/README.md`
- Create: `scripts/apply-linux-patches.sh`
- Create: `tests/test_linux_patches.sh`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**

```text
scripts/apply-linux-patches.sh <linux-source-dir>
  0: every tracked patch is already applied or was applied successfully
  nonzero: source version mismatch, dirty partial application, or invalid patch
```

- [ ] Write a fixture-based shell test that creates a tiny temporary source tree and two ordered patches, invokes the real patch applier twice, and verifies identical content after both runs.
- [ ] Add failure cases for a missing source directory and a patch that is neither forward-applicable nor reverse-applicable.
- [ ] Run `./tests/test_linux_patches.sh`; expect failure because the applier does not exist.
- [ ] Implement ordered `*.patch` discovery, forward `git apply --check`, reverse `git apply --reverse --check`, and strict failure diagnostics.
- [ ] Make `apply-linux-overlay.sh` call the patch applier before installing Q3N overlay files.
- [ ] Add the new behavioral test to `scripts/smoke-test.sh`.
- [ ] Run `./tests/test_linux_patches.sh` and `./scripts/smoke-test.sh`; expect both to pass.
- [ ] Commit: `test: add idempotent Linux patch pipeline`

## Task 2: Patch raw NAND geometry representation for exact division

**Files:**
- Create: `linux/patches/0001-mtd-rawnand-add-exact-geometry-helpers.patch`
- Modify: `tests/test_linux_patches.sh`

**Interfaces added by the patch:**

```c
#define NAND_NON_POWER_OF_2_GEOMETRY BIT(15)

static inline bool nand_has_non_power_of_2_geometry(const struct nand_chip *chip);
static inline u64 nand_page_to_offs(const struct nand_chip *chip, u64 page);
static inline u64 nand_offs_to_page(const struct nand_chip *chip, loff_t offs);
static inline u64 nand_offs_to_eraseblock(const struct nand_chip *chip, loff_t offs);
static inline u32 nand_offs_in_eraseblock(const struct nand_chip *chip, loff_t offs);
```

- [ ] Add a real-patch test that copies only the required pristine Linux 7.0.12 files to a temporary tree, applies patch 0001, compiles a small userspace arithmetic harness mirroring the exported helper contract, and checks pages 0, 1599, 1600, and 2,662,399.
- [ ] Check literal expected offsets: page 1599 → 26,198,016; page 1600 → 26,214,400; final page → 43,620,745,216 and stays below 43,620,761,600 bytes.
- [ ] Run the test before creating the patch; expect failure because the option and helpers are missing.
- [ ] Create patch 0001 against pristine Linux 7.0.12 `include/linux/mtd/rawnand.h` and raw-NAND internals, using multiplication/division only when the option bit is set and retaining the existing fast path otherwise.
- [ ] Validate forward apply, reverse detection, and whitespace with `git apply --check` and `git apply --check --whitespace=error`.
- [ ] Run `./tests/test_linux_patches.sh`; expect pass.
- [ ] Commit: `mtd: add exact raw NAND geometry helpers patch`

## Task 3: Patch `nand_base.c` to use exact page/block conversions

**Files:**
- Create: `linux/patches/0002-mtd-rawnand-use-exact-geometry-in-nand-base.patch`
- Modify: `tests/test_linux_patches.sh`

**Behavior covered:**

- Exact offset/length validation for a 25 MiB eraseblock.
- Page and block selection without `page_shift`, `phys_erase_shift`, `chip_shift`, or bit masks when the new option is active.
- Read, write, OOB, erase, bad-block-marker page selection, cached page, and row-cycle calculations.
- Existing behavior remains unchanged for conventional power-of-two devices.

- [ ] Extend the patched-tree test to build a focused KUnit-compatible helper harness covering reads crossing page 1599→1600, erase at block 1663, and rejection of erase offset +1.
- [ ] Run the test with patch 0001 only; expect at least the block-alignment and page-selection cases to fail.
- [ ] Create patch 0002 against Linux 7.0.12 `drivers/mtd/nand/raw/nand_base.c`, replacing all non-power-sensitive shifts and masks with conditional exact helpers.
- [ ] Inspect every remaining `writesize - 1`, `erasesize - 1`, `page_shift`, `phys_erase_shift`, `chip_shift`, and `pagemask` occurrence and document why it is safe or convert it.
- [ ] Validate patch apply/reverse/whitespace and run `./tests/test_linux_patches.sh`.
- [ ] Commit: `mtd: use exact geometry in raw NAND core patch`

## Task 4: Patch `nand_bbt.c` for a 1600-page eraseblock

**Files:**
- Create: `linux/patches/0003-mtd-rawnand-use-exact-geometry-in-bbt.patch`
- Modify: `tests/test_linux_patches.sh`

**Behavior covered:**

- 1664 eraseblocks produce a 416-byte two-bit RAM BBT.
- Block offset, page number, scan step, per-target selection, and BBM scan use exact arithmetic.
- OOB byte zero remains the persistent bad-block marker.

- [ ] Extend the patched-tree harness with literal cases for BBT entries 0, 3, 4, and 1663 and assert an allocated size of 416 bytes.
- [ ] Add BBM-page cases for first-page and last-page conventions in a 1600-page block.
- [ ] Run with patches 0001–0002; expect BBT block/page cases to fail.
- [ ] Create patch 0003 against Linux 7.0.12 `drivers/mtd/nand/raw/nand_bbt.c`, conditionally replacing power-of-two assumptions.
- [ ] Inventory all remaining shift/mask geometry operations in `nand_bbt.c`.
- [ ] Validate all three patches on a clean temporary Linux tree, then invoke the applier twice.
- [ ] Commit: `mtd: support exact geometry in NAND BBT patch`

## Task 5: Introduce host-testable Q3N geometry and address conversion

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_addr.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_addr.c`
- Create: `tests/test_q3n_addr.c`
- Create: `tests/test_q3n_addr.sh`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**

```c
struct q3n_geometry {
	u32 writesize;
	u32 oobsize;
	u32 pages_per_block;
	u32 blocks;
};

int q3n_page_to_address(const struct q3n_geometry *geo, u32 page,
			u32 *block, u32 *page_in_block);
int q3n_offset_to_address(const struct q3n_geometry *geo, u64 offset,
			  u32 *block, u32 *page_in_block, u32 *column);
```

- [ ] Write table-driven host tests with literal boundaries: pages 0/1599/1600/2,662,399, offset 26,214,400, and out-of-range inputs.
- [ ] Run `./tests/test_q3n_addr.sh`; expect compilation failure because the API does not exist.
- [ ] Implement the pure arithmetic without kernel dependencies; expose kernel type aliases through a test shim only.
- [ ] Run the test and mutation-check wrong divisor, wrong upper bound, and truncated 64-bit offset.
- [ ] Commit: `test: add exact Q3N address conversion`

## Task 6: Add an independent YTMC flash-ID provider

**Files:**
- Create: `linux/drivers/mtd/nand/raw/ytmc_nand.h`
- Create: `linux/drivers/mtd/nand/raw/ytmc_nand.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_flash.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_flash.c`
- Create: `tests/test_q3n_flash.c`
- Create: `tests/test_q3n_flash.sh`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**

```c
#define YTMC_Q3N_ID_LEN 8

const struct nand_flash_dev *ytmc_nand_ids(void);
const struct nand_flash_dev *q3n_flash_ids_for_id(const u8 *id, size_t len);
```

- [ ] Write a host contract test that passes the full ID `9c d7 98 a6 51 33 4e 44`, seven-byte prefixes, one-byte mutations at every position, unknown IDs, and NULL/zero length.
- [ ] Assert the selected profile reports 16 KiB page, 1 KiB OOB, 1600 pages/block, 1664 blocks, and the non-power geometry option.
- [ ] Run `./tests/test_q3n_flash.sh`; expect missing-source failure.
- [ ] Implement an exact eight-byte selector in `qemu_3dnand_flash.c` and a sentinel-terminated `struct nand_flash_dev` table in `ytmc_nand.c`.
- [ ] Keep manufacturer-specific data out of controller files.
- [ ] Run the test and commit: `mtd: add YTMC NAND ID whitelist provider`

## Task 7: Extend QEMU with the YTMC ID and deterministic read retry

**Files:**
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `tests/test_q3n_controller.c`
- Modify: `tests/test_q3n_overlay.sh`

**Register/behavior contract:**

```c
Q3N_REG_RETRY_MODE
Q3N_REG_READ_FLAGS
#define Q3N_READ_F_RAW BIT(0)
```

- `READ ID` returns `9c d7 98 a6 51 33 4e 44`.
- Corrected error count is `max(raw_errors - retry_gain[mode], 0)`.
- Raw reads return overlay-corrupted main data and do not update the ECC result.
- Error bands 0–40 succeed at mode 0, 41–48 at mode 1, 49–56 at mode 2, 57–64 at mode 3, and 65+ remain uncorrectable.

- [ ] Add literal table-driven tests for all boundary values 0, 40, 41, 48, 49, 56, 57, 64, and 65 across retry modes.
- [ ] Add a state test proving raw reads leave the prior ECC result unchanged.
- [ ] Add a reset test proving retry mode returns to zero.
- [ ] Run `./tests/test_q3n_overlay.sh`; expect new assertions to fail.
- [ ] Implement pure retry/error helpers inside the existing extraction markers, then wire MMIO state and raw-read behavior.
- [ ] Run host tests with sanitizers where supported and commit: `hw/mtd: model Q3N read retry and raw reads`

## Task 8: Create the register-only hardware implementation layer

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_regs.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_hw.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_hw.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h` (replace legacy contents)
- Create: `tests/test_q3n_hw.c`
- Create: `tests/test_q3n_hw.sh`

**Interfaces:**

```c
int q3n_hw_reset(struct q3n *q3n);
int q3n_hw_read_id(struct q3n *q3n, u8 *id, size_t len);
int q3n_hw_read_page(struct q3n *q3n, u32 page, void *data, void *oob,
		     bool raw, struct q3n_ecc_result *result);
int q3n_hw_program_page(struct q3n *q3n, u32 page,
			const void *data, const void *oob);
int q3n_hw_erase_block(struct q3n *q3n, u32 block);
int q3n_hw_set_retry_mode(struct q3n *q3n, unsigned int mode);
```

- [ ] Build a fake-MMIO host harness that records offset/value accesses and emulates completion/status.
- [ ] Test exact register sequencing for reset, ID, corrected read, raw read, program, erase, and retry mode.
- [ ] Test timeout and controller-error propagation.
- [ ] Run `./tests/test_q3n_hw.sh`; expect missing-source failure.
- [ ] Implement all MMIO access in `qemu_3dnand_hw.c`; inject read/write hooks in host builds without changing the production ABI.
- [ ] Confirm no other production Q3N `.c` contains `readl(`, `writel(`, or direct BAR arithmetic.
- [ ] Commit: `mtd: add Q3N register access layer`

## Task 9: Implement NAND controller `exec_op`

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_controller.c`
- Create: `tests/test_q3n_exec_op.c`
- Create: `tests/test_q3n_exec_op.sh`

**Supported operations:**

- reset;
- read ID;
- read status;
- page read command/address/wait/data sequence;
- page program command/address/data/confirm/wait sequence;
- block erase command/row/confirm/wait sequence;
- change/read column as required by core OOB operations.

**Interfaces:**

```c
extern const struct nand_controller_ops q3n_controller_ops;
int q3n_exec_op(struct nand_chip *chip,
		const struct nand_operation *op, bool check_only);
```

- [ ] Write behavior tests using complete `nand_operation` fixtures and a fake hardware boundary, covering supported sequences, malformed instruction order, oversized address cycles, unsupported target, and `check_only`.
- [ ] Run `./tests/test_q3n_exec_op.sh`; expect missing implementation failure.
- [ ] Implement a bounded parser that validates before issuing hardware operations; `check_only` must perform no side effects.
- [ ] Return `-ENOTSUPP` for unsupported but valid NAND operations and `-EINVAL` for malformed Q3N sequences.
- [ ] Run tests and commit: `mtd: implement Q3N NAND controller operations`

## Task 10: Implement ECC, raw, OOB, and read-retry callbacks

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_ecc.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_ecc.h`
- Create: `tests/test_q3n_ecc.c`
- Create: `tests/test_q3n_ecc.sh`

**Interfaces:**

```c
int q3n_ecc_init(struct q3n *q3n);
int q3n_ecc_read_page(struct nand_chip *chip, u8 *buf,
		      int oob_required, int page);
int q3n_ecc_write_page(struct nand_chip *chip, const u8 *buf,
		       int oob_required, int page);
int q3n_ecc_read_page_raw(struct nand_chip *chip, u8 *buf,
			  int oob_required, int page);
int q3n_ecc_write_page_raw(struct nand_chip *chip, const u8 *buf,
			   int oob_required, int page);
int q3n_ecc_read_oob(struct nand_chip *chip, int page);
int q3n_ecc_write_oob(struct nand_chip *chip, int page);
int q3n_setup_read_retry(struct nand_chip *chip, int retry_mode);
```

- [ ] Write fake-hardware tests for clean reads, corrected counts, threshold return, uncorrectable ECC accounting, raw/OOB routing, hardware errors, invalid retry mode, and reset to mode zero.
- [ ] Explicitly simulate NAND Core retry: call mode 0 through 3 after each non-negative failed attempt and verify bands 41–64 recover at the documented mode.
- [ ] Verify an uncorrectable attempt increments `ecc_stats.failed`, returns a non-negative threshold value, and never returns `-EBADMSG`.
- [ ] Run `./tests/test_q3n_ecc.sh`; expect missing implementation failure.
- [ ] Implement callbacks and set `chip->ecc.engine_type`, size/strength/step geometry, callback table, `chip->read_retries = 4`, and `chip->ops.setup_read_retry`.
- [ ] Run tests and commit: `mtd: implement Q3N ECC and read retry callbacks`

## Task 11: Wire module, PCI probe, and `nand_scan_with_ids()`

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_module.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_init.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_init.h`
- Create: `tests/test_q3n_probe.c`
- Create: `tests/test_q3n_probe.sh`

**Initialization sequence:**

```text
PCI probe → enable/map BAR → nand_controller_init
→ hardware reset/read full ID → exact provider selection
→ set controller data/options/ECC callbacks
→ nand_scan_with_ids(chip, 1, ids)
→ mtd_device_register
```

- [ ] Write a link-time probe harness with complete fake PCI, NAND scan, and MTD registration boundaries.
- [ ] Verify call order, exact ID-table pointer passed to `nand_scan_with_ids`, maxchips=1, cleanup after each failure point, and successful remove.
- [ ] Verify scan failure is not followed by `nand_cleanup`, while post-scan failures and remove are.
- [ ] Run `./tests/test_q3n_probe.sh`; expect missing implementation failure.
- [ ] Implement module-only PCI ID registration in `qemu_3dnand_module.c` and lifecycle logic in `qemu_3dnand_init.c`.
- [ ] Do not set any MTD callback and do not set `NAND_SKIP_BBTSCAN`.
- [ ] Run tests and commit: `mtd: initialize Q3N through nand_scan_with_ids`

## Task 12: Replace the legacy module composition

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `linux/drivers/mtd/nand/raw/Kconfig.qemu_3dnand`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `tests/test_scripts.sh`
- Delete: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Delete: `linux/drivers/mtd/nand/raw/qemu_3dnand_map.c`
- Delete: `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`
- Delete: `linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c`
- Delete: `linux/drivers/mtd/nand/raw/qemu_3dnand.h`

**Production object list:**

```make
qemu_3dnand-y := qemu_3dnand_module.o qemu_3dnand_init.o \
		 qemu_3dnand_flash.o ytmc_nand.o \
		 qemu_3dnand_controller.o qemu_3dnand_ecc.o \
		 qemu_3dnand_addr.o qemu_3dnand_hw.o
```

- [ ] Rewrite script tests around observable overlay results in a temporary Linux tree: expected new files exist, production Makefile links the exact modules, a second overlay run is stable, and legacy RAID objects are absent.
- [ ] Run `./tests/test_scripts.sh`; expect failure against the legacy composition.
- [ ] Update Kconfig/Makefile and overlay copy list, migrate useful KUnit arithmetic/ECC tests, then remove obsolete source files.
- [ ] Run script and host tests.
- [ ] Commit: `build: switch Q3N module to NAND Core architecture`

## Task 13: Verify NAND Core bad-block and BBT ownership

**Files:**
- Create: `tests/test_q3n_nand_core_contract.sh`
- Modify: `scripts/smoke-test.sh`
- Modify: `docs/superpowers/specs/2026-07-28-q3n-nand-core-ecc-read-retry-design.md` if implementation details differ

**Behavior covered:**

- A patched Linux build reaches `nand_scan_tail()`.
- MTD bad-block operations resolve to NAND Core callbacks.
- Initial BBT scan allocates 416 bytes for 1664 blocks.
- Markbad persists OOB byte zero through the driver's OOB callback and is visible after rescan.

- [ ] Build a focused kernel-link or KUnit test using the actual patched `nand_base.c`, `nand_bbt.c`, and Q3N callbacks with a memory-backed hardware boundary.
- [ ] Demonstrate RED by temporarily omitting BBT scan or the OOB write callback in the test configuration.
- [ ] Add only the missing driver wiring needed for NAND Core to own the behavior.
- [ ] Assert the module has no private BBT symbol and no direct MTD callback assignment through a compiled/link contract, not a source-text check.
- [ ] Run the contract test and commit: `test: verify NAND Core BBT ownership`

## Task 14: Build the patched Linux module and QEMU model

**Files:**
- Modify as required by compiler diagnostics, restricted to Q3N overlays, patch files, and scripts.
- Modify: `scripts/build-module.sh`
- Modify: `scripts/build-qemu.sh`

- [ ] Apply all Linux patches and overlays to a fresh Linux 7.0.12 source copy.
- [ ] Configure `CONFIG_MTD_RAW_NAND=y`, `CONFIG_MTD_NAND_QEMU_3DNAND=m`, and KUnit test options.
- [ ] Build the module with warnings enabled; record and fix every new Q3N warning.
- [ ] Apply the QEMU overlay to a fresh QEMU 11.0.2 source copy and build the device model.
- [ ] Run KUnit and all host tests.
- [ ] Commit compiler/API fixes: `build: validate Q3N against Linux 7.0 and QEMU 11`

## Task 15: Run guest integration for geometry, I/O, BBT, and retry

**Files:**
- Modify: `scripts/q3n-serial-smoke.sh`
- Create: `scripts/q3n-read-retry-smoke.sh`
- Modify: `scripts/q3n-persistence-smoke.sh`
- Modify: `scripts/smoke-test.sh`

**Guest assertions:**

```text
writesize = 16384
oobsize = 1024
erasesize = 26214400
size = 43620761600
blocks = 1664
pages = 2662400
```

- [ ] Add guest tests for first/last page, page 1599→1600 boundary, first/last eraseblock, unaligned erase rejection, OOB byte-zero markbad, BBT rescan, and UBI attach.
- [ ] Add deterministic retry injections for error counts 40, 41, 49, 57, and 65 and verify corrected/failed statistics plus retry-mode reset.
- [ ] Add a raw-read assertion that injected corruption is visible without changing ECC result state.
- [ ] Run the new smoke tests before final integration; expect failures until the guest/model paths are wired.
- [ ] Fix only observed integration defects and rerun each focused test.
- [ ] Commit: `test: cover Q3N NAND Core guest integration`

## Task 16: Final verification, documentation reconciliation, and GitHub update

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-07-28-q3n-nand-core-ecc-read-retry-design.md`
- Modify: this plan to check completed tasks

- [ ] Reconcile implemented interfaces and limitations with the design document; keep page RAID explicitly out of scope.
- [ ] Document patch order, supported Linux/QEMU versions, build commands, ID whitelist extension procedure, and read-retry fault injection.
- [ ] Run `git diff --check`.
- [ ] Run all host tests and `./scripts/smoke-test.sh`.
- [ ] Apply patches/overlays twice to fresh trees and rebuild Linux module plus QEMU.
- [ ] Run guest serial, persistence, BBT, and read-retry smoke tests.
- [ ] Use `superpowers:verification-before-completion` and capture fresh command output.
- [ ] Use `superpowers:requesting-code-review`; address findings using `superpowers:receiving-code-review`.
- [ ] Commit: `docs: complete Q3N NAND Core implementation`
- [ ] Push `codex/nand-core-ecc-read-retry` and update the existing draft GitHub pull request with implementation status and verification results.
