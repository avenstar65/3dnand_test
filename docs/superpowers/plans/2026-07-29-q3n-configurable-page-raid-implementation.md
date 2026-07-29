# Q3N Configurable Page RAID Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an optional Linux-driver Page RAID layer that exposes one logical NAND page as 2, 4, or 8 physical data pages plus one driver-generated XOR parity page while preserving the current one-logical-page/one-physical-page path when disabled.

**Architecture:** `qemu_3dnand_layout.c` owns pure geometry and logical-to-physical mapping. `qemu_3dnand_page.c` is the stable logical page boundary used by ECC and legacy erase code; it selects identity physical ops or the conditionally linked synchronous RAID ops in `qemu_3dnand_page_raid.c`. The hardware layer and QEMU keep their existing single-physical-page ABI, and NAND Core continues to own MTD entry points, read-retry iteration, bad-block callbacks, and RAM BBT.

**Tech Stack:** Linux 7.0.12 raw NAND/MTD APIs, C11 host behavior tests, POSIX shell build tests, existing Q3N MMIO helpers, QEMU 11.0.2.

## Execution Status

| Task | Status | Evidence |
| --- | --- | --- |
| 1: configuration/layout/scan IDs | Complete | `f857763`; disabled and 2:1/4:1/8:1 literal layout tests |
| 2: logical page and synchronous RAID | Complete | `aea0747`; identity, parity order, recovery, erased-main/OOB-only tests |
| 3: NAND Core consumers | Complete | `7c4e357`; default 4:1 full Linux build and compiled contract |
| 4: profiles/guest/docs | Complete | exact-geometry helper harness; four `.ko` profiles; 4:1 persistence/BBT and 8:1 tail guest acceptance |

Task 4 module outputs:

```text
work/build/linux-7.0.12/drivers/mtd/nand/raw/qemu_3dnand.ko
work/task4-build/disabled/linux-7.0.12/drivers/mtd/nand/raw/qemu_3dnand.ko
work/task4-build/2/linux-7.0.12/drivers/mtd/nand/raw/qemu_3dnand.ko
work/task4-build/8/linux-7.0.12/drivers/mtd/nand/raw/qemu_3dnand.ko
```

The isolated profile builds used `scripts/config` plus `olddefconfig`, built
`drivers/mtd/nand/raw/`, and then ran `scripts/build-kernel.sh` and the compiled
contract with the profile output selected. The default 4:1 guest ran
`scripts/q3n-persistence-smoke.sh` and a profile acceptance boot. The 8:1 guest
ran the repository QEMU with `BUILD_DIR=/workspace/work/task4-build/8`, a fresh
image, and the 8:1 initramfs. Guest fault injection is not claimed because no
guest-facing injection interface exists.

## Global Constraints

- Work only on `codex/nand-core-ecc-read-retry` in the existing isolated worktree.
- Do not register `exec_op`; keep the traditional `cmdfunc`/`waitfunc` controller path.
- Do not assign direct Q3N MTD read/write/erase/bad-block callbacks.
- `_block_isbad`, `_block_markbad`, `_block_isreserved`, and RAM BBT remain owned by raw NAND Core.
- `q3n_hw_*` keeps single-physical-page or single-physical-block semantics.
- Do not change QEMU MMIO ABI or calculate parity in QEMU.
- Page RAID enable and data-page ratio are two independent Kconfig settings.
- RAID data-page count is compile-time 2, 4, or 8 and must pass a runtime power-of-two validation.
- One logical stripe is `N` consecutive 16 KiB data pages followed by one 16 KiB parity page.
- A stripe never crosses a 1600-page physical block; incomplete tail pages are inaccessible.
- Program order is `D0..DN-1,P` under one driver lock. No rollback or power-fail atomicity is promised.
- Before programming any physical main-data page, test its complete 16 KiB
  buffer. If every byte is `0xff`, skip the main-data PROGRAM command. If
  that data page has non-erased OOB, issue only the OOB program; if its OOB
  is also all `0xff` or absent, skip the physical page entirely. Apply the
  same main-data rule to parity after parity has been calculated.
- Parity is main-data XOR only. Do not add parity metadata, commit markers, generations, CRC, caches, workers, or schedulers.
- Logical OOB is the concatenation of the `N` data-page OOBs. Parity OOB stays erased and hidden.
- Raw write still calculates parity. Raw read does not read parity, retry, or recover.
- Normal read uses NAND Core retry modes first. Only the final mode may reconstruct exactly one failed data page.
- Linux raw-NAND source changes remain represented by `linux/patches/`; do not hand-edit `work/linux/linux-7.0.12`.
- Every production behavior follows RED → GREEN → REFACTOR with a focused behavioral test.

---

## Task 1: Add Page RAID configuration, logical geometry, and scan-ID derivation

**Files:**

- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_layout.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_layout.c`
- Create: `tests/test_q3n_layout.c`
- Create: `tests/test_q3n_layout.sh`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_flash.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_flash.c`
- Modify: `tests/test_q3n_flash.c`
- Modify: `linux/drivers/mtd/nand/raw/Kconfig.qemu_3dnand`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**

```c
struct q3n_page_profile {
	bool raid_enabled;
	u32 data_pages;
	u32 parity_pages;
	u32 stripe_pages;
	u32 stripes_per_block;
	u32 tail_pages;
	struct q3n_geometry physical;
	struct q3n_geometry logical;
	u64 logical_size;
};

struct q3n_page_map {
	u32 logical_block;
	u32 stripe_in_block;
	u32 first_data_page;
	u32 parity_page;
};

int q3n_layout_build(struct q3n_page_profile *profile,
		     const struct q3n_geometry *physical,
		     bool raid_enabled, u32 data_pages);
int q3n_layout_map_page(const struct q3n_page_profile *profile,
			u32 logical_page, struct q3n_page_map *map);
int q3n_flash_build_scan_ids(struct nand_flash_dev scan_ids[2],
			     const struct nand_flash_dev *physical_ids,
			     const struct q3n_page_profile *profile);
```

**Literal profile expectations:**

| Profile | logical page | logical OOB | pages/block | tail | erasesize | chipsize MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| disabled | 16384 | 1024 | 1600 | 0 | 26214400 | 41600 |
| 2:1 | 32768 | 2048 | 533 | 1 | 17465344 | 27716 |
| 4:1 | 65536 | 4096 | 320 | 0 | 20971520 | 33280 |
| 8:1 | 131072 | 8192 | 177 | 7 | 23199744 | 36816 |

- [ ] **Step 1: Write the failing layout behavior test.**

  Add table-driven literals for all four profiles, plus `N=0,1,3,5,6,7,16`,
  zero geometry, multiplication overflow, first/last stripe, next-block
  stripe, and out-of-range logical page. The mapping assertions must include:

  ```c
  /* 4:1 */
  expect_map(0, 0, 4);
  expect_map(319, 1595, 1599);
  expect_map(320, 1600, 1604);

  /* 8:1 */
  expect_map(176, 1584, 1592);
  expect_map(177, 1600, 1608);

  /* 2:1 */
  expect_map(532, 1596, 1598);
  expect_map(533, 1600, 1602);
  ```

- [ ] **Step 2: Run the new layout test and verify RED.**

  Run:

  ```sh
  ./tests/test_q3n_layout.sh
  ```

  Expected: compilation fails because `qemu_3dnand_layout.h/.c` do not exist.

- [ ] **Step 3: Implement the minimum pure layout layer.**

  Use checked 64-bit multiplication and exact division:

  ```c
  stripes = physical->pages_per_block / (data_pages + 1);
  tail = physical->pages_per_block - stripes * (data_pages + 1);
  logical.writesize = physical->writesize * data_pages;
  logical.oobsize = physical->oobsize * data_pages;
  logical.pages_per_block = stripes;
  logical.blocks = physical->blocks;
  logical_size = logical.writesize * stripes * logical.blocks;
  ```

  Disabled mode must set `data_pages=1`, `parity_pages=0`,
  `stripe_pages=1`, copy the physical geometry, and map logical page to the
  identical physical page.

- [ ] **Step 4: Run the layout test and verify GREEN, then mutation-check.**

  Run `./tests/test_q3n_layout.sh`. Temporarily changing `(N + 1)` to `N`,
  accepting `N=3`, or allowing a tail-page map must make a focused case fail;
  restore the implementation and rerun GREEN.

- [ ] **Step 5: Add the two Kconfig settings and failing scan-ID tests.**

  Add:

  ```text
  config MTD_NAND_QEMU_3DNAND_PAGE_RAID
      bool "QEMU 3D NAND driver-side Page RAID"
      depends on MTD_NAND_QEMU_3DNAND

  config MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES
      int "Page RAID data pages per parity page"
      range 2 8
      default 4
      depends on MTD_NAND_QEMU_3DNAND_PAGE_RAID
  ```

  Extend the real flash provider test so `q3n_flash_build_scan_ids()` must
  copy the complete 8-byte ID, `id_len`, ECC requirement, options, and name
  from `ytmc_nand.c`, replace only logical geometry fields, and add an empty
  sentinel entry. Verify all four chipsize-MiB literals above.

- [ ] **Step 6: Run the flash test and verify RED.**

  Run `./tests/test_q3n_flash.sh`; expected failure is an undefined
  `q3n_flash_build_scan_ids`.

- [ ] **Step 7: Implement device-local scan-ID derivation and verify GREEN.**

  Reject null arguments, non-integral MiB logical capacity, and values that
  do not fit the `nand_flash_dev` fields. Do not modify the static
  `ytmc_nand.c` table.

- [ ] **Step 8: Add the layout test to smoke and run focused regression.**

  Run:

  ```sh
  ./tests/test_q3n_layout.sh
  ./tests/test_q3n_flash.sh
  ./scripts/smoke-test.sh
  ```

  Expected: all pass.

- [ ] **Step 9: Commit Task 1.**

  ```sh
  git add linux/drivers/mtd/nand/raw/qemu_3dnand_layout.* \
    linux/drivers/mtd/nand/raw/qemu_3dnand_flash.* \
    linux/drivers/mtd/nand/raw/Kconfig.qemu_3dnand \
    tests/test_q3n_layout.* tests/test_q3n_flash.c scripts/smoke-test.sh
  git commit -m "mtd: add Q3N page RAID layout profiles"
  ```

---

## Task 2: Add the stable logical-page layer and synchronous RAID behavior

**Files:**

- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_page.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_page.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.c`
- Create: `tests/test_q3n_page.c`
- Create: `tests/test_q3n_page.sh`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**

```c
struct q3n_page_result {
	u32 max_bitflips;
	u32 corrected_bits;
	u32 failed_data_pages;
	bool parity_failed;
	bool recovered;
};

int q3n_page_layer_init(struct q3n *q3n, bool raid_enabled, u32 data_pages);
void q3n_page_layer_cleanup(struct q3n *q3n);
int q3n_page_read(struct q3n *q3n, u32 logical_page,
		  void *data, void *oob, bool raw,
		  struct q3n_page_result *result);
int q3n_page_write(struct q3n *q3n, u32 logical_page,
		   const void *data, const void *oob);
int q3n_page_read_oob(struct q3n *q3n, u32 logical_page, void *oob);
int q3n_page_write_oob(struct q3n *q3n, u32 logical_page, const void *oob);
int q3n_page_erase_block(struct q3n *q3n, u32 logical_block);
```

`q3n_page_*` functions require the caller to hold `q3n->lock`; they must not
lock recursively. `q3n_hw_*` remains the only physical-I/O dependency.

- [ ] **Step 1: Write the failing identity-path tests.**

  Compile the real page layer against fake `q3n_hw_*` functions. With RAID
  disabled, assert that logical page/block values and buffers are forwarded
  exactly once, page/OOB results are preserved, and no parity scratch or
  RAID symbol is required.

- [ ] **Step 2: Write the failing RAID write/OOB/erase tests.**

  Use four 16 KiB slices with hand-derived byte patterns. Assert:

  ```text
  calls: PROGRAM D0, D1, D2, D3, P4
  parity[j] = D0[j] XOR D1[j] XOR D2[j] XOR D3[j]
  parity OOB argument = NULL
  logical OOB chunks -> D0.oob, D1.oob, D2.oob, D3.oob
  erase logical block 7 -> physical block 7 exactly once
  ```

  Add failure fixtures for D0, middle data, final data, and parity. The call
  log must stop at the first failure, and no failed write may be reported as
  success.

- [ ] **Step 2a: Add erased-physical-page skip tests.**

  Prove the optimization through the real page layer:

  ```text
  identity data=all-ff, OOB absent/all-ff -> no hardware program
  identity data=all-ff, OOB non-ff -> OOB-only program
  RAID data slice=all-ff, OOB absent/all-ff -> that D program is omitted
  RAID data slice=all-ff, OOB non-ff -> only that D OOB is programmed
  computed parity=all-ff -> P program is omitted
  computed parity contains any non-ff byte -> P remains last program
  ```

  The test must scan every byte, not only the first/last word. Include a
  buffer that is all `0xff` except one middle byte and assert it is
  programmed.

- [ ] **Step 3: Write the failing RAID read/recovery tests.**

  Cover:

  - all data pages clean;
  - corrected counts aggregate and maximum bitflips is selected;
  - one uncorrectable data page before final retry does not read parity;
  - one uncorrectable data page at retry mode 3 reads parity and reconstructs
    the exact missing 16 KiB literal;
  - two failed data pages do not read parity;
  - failed parity does not recover;
  - any negative physical read error is returned unchanged;
  - raw read concatenates data pages and never reads parity;
  - raw write still emits parity.

- [ ] **Step 4: Run the page test and verify RED.**

  Run `./tests/test_q3n_page.sh`; expected failure is missing page-layer
  sources and symbols.

- [ ] **Step 5: Implement the identity ops and minimal page dispatcher.**

  Store in `struct q3n`:

  ```c
  struct q3n_geometry physical_geometry;
  struct q3n_geometry geometry; /* NAND Core logical geometry */
  struct q3n_page_profile page_profile;
  const struct q3n_page_ops *page_ops;
  u8 *parity_scratch;
  u64 raid_recovered_pages;
  struct nand_flash_dev scan_ids[2]; /* kernel build */
  ```

  In disabled builds, use an inline/conditional stub so the module has no
  undefined reference to `qemu_3dnand_page_raid.o`.

- [ ] **Step 6: Implement synchronous RAID write, OOB, erase, and read.**

  XOR exactly `Q3N_PAGE_SIZE` bytes into the 16 KiB scratch buffer. Program
  data pages first and parity last. Before each physical main-data program,
  use one shared erased-buffer helper over all 16 KiB. Skip all-`0xff` main
  data; preserve non-erased OOB with `q3n_hw_program_oob()`, and skip an
  all-`0xff` OOB-only request. Compute parity from every data slice whether
  or not its physical program was skipped, then apply the same all-`0xff`
  main-data skip to the parity page. For normal reads, aggregate only data
  ECC counters; on mode 3 and exactly one failed data page, read parity with
  ECC and reconstruct the missing slice:

  ```c
  missing = parity;
  for each good data slice:
      missing ^= data_slice;
  ```

  Set `result->recovered=true`, clear `failed_data_pages`, force
  `max_bitflips >= Q3N_ECC_STRENGTH`, and increment
  `raid_recovered_pages`. Do not write recovered data back.

- [ ] **Step 7: Run the page test and verify GREEN, then mutation-check.**

  Run `./tests/test_q3n_page.sh`. Wrong parity order, skipped slice, recovery
  with two failures, parity exposure in raw read, and a remapped erase block
  must each be caught by a focused assertion.

- [ ] **Step 8: Wire object composition and overlay copying.**

  Always link `qemu_3dnand_page.o` and `qemu_3dnand_layout.o`. Link:

  ```make
  qemu_3dnand-$(CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID) += \
	  qemu_3dnand_page_raid.o
  ```

  Copy all six new page/layout headers and sources in
  `scripts/apply-linux-overlay.sh`.

- [ ] **Step 9: Run focused and smoke regression.**

  ```sh
  ./tests/test_q3n_page.sh
  ./tests/test_q3n_layout.sh
  ./scripts/smoke-test.sh
  ```

- [ ] **Step 10: Commit Task 2.**

  ```sh
  git add linux/drivers/mtd/nand/raw/qemu_3dnand_page* \
    linux/drivers/mtd/nand/raw/qemu_3dnand_layout* \
    linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
    linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
    scripts/apply-linux-overlay.sh scripts/smoke-test.sh \
    tests/test_q3n_page.*
  git commit -m "mtd: implement synchronous Q3N page RAID"
  ```

---

## Task 3: Route ECC, OOB, erase, and initialization through the logical-page layer

**Files:**

- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_ecc.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_ecc.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_controller.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_init.c`
- Modify: `tests/test_q3n_ecc.c`
- Modify: `tests/test_q3n_legacy.c`
- Modify: `tests/test_q3n_nand_core_contract.sh`
- Modify: `configs/linux/mtd.fragment`

**Interfaces:**

```c
int q3n_ecc_account_page_result(const struct q3n_page_result *result,
				unsigned int retry_mode,
				struct q3n_ecc_stats *stats);
```

Initialization order:

```text
reset -> full-ID physical whitelist match
-> q3n_page_layer_init(config enabled, config ratio)
-> q3n_flash_build_scan_ids()
-> nand_scan_with_ids(chip, 1, q3n->scan_ids)
-> attach validates logical geometry and installs ECC callbacks
-> print explicit Page RAID profile
-> mtd_device_register()
```

- [ ] **Step 1: Extend ECC accounting tests and verify RED.**

  Add literal `q3n_page_result` cases:

  ```text
  clean/corrected -> corrected sum and max bitflips
  one failed data page -> failed increments once, non-negative strength
  recovered at mode 3 -> failed unchanged, return strength
  retry success below threshold -> return strength
  ```

  Run `./tests/test_q3n_ecc.sh`; expected undefined page-result accounting.

- [ ] **Step 2: Extend legacy erase tests and verify RED.**

  Use a 4:1 logical geometry (`320 pages/block`) and assert ERASE1 row 320
  maps through `q3n_page_erase_block()` to physical block 1. Row 319 must be
  rejected. The test must fail while controller code calls hardware erase
  directly.

- [ ] **Step 3: Extend the compiled NAND-Core contract before integration.**

  Require `q3n_page_read`, `q3n_page_write`, `q3n_page_read_oob`,
  `q3n_page_write_oob`, and `q3n_page_erase_block` in the built module.
  Require no direct MTD callback, no `exec_op`, and no private BBT as before.
  When RAID is disabled, require absence of `q3n_page_raid_ops`; when enabled,
  require its presence.

- [ ] **Step 4: Implement page-result ECC accounting and route callbacks.**

  Replace direct `q3n_hw_read/program/read_oob/program_oob` calls in
  `qemu_3dnand_ecc.c` with `q3n_page_*` calls while keeping one outer
  `q3n->lock` critical section. Set:

  ```c
  chip->ecc.steps = mtd->writesize / Q3N_ECC_STEP_SIZE;
  ```

  Do not use the fixed physical page size for logical ECC steps.

- [ ] **Step 5: Route legacy erase through the page layer and verify GREEN.**

  `ERASE1` validates logical rows using `q3n->geometry.pages_per_block`;
  `ERASE2` computes logical block and calls `q3n_page_erase_block()`.
  Run `./tests/test_q3n_legacy.sh`.

- [ ] **Step 6: Integrate profile selection and device-local scan IDs.**

  In kernel builds, call:

  ```c
  raid_enabled = IS_ENABLED(CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID);
  data_pages = raid_enabled ?
      CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES : 1;
  ```

  Save the physical geometry before creating logical geometry. Reject any
  scan result that differs from the selected profile. Allocate/release
  scratch through the page-layer lifecycle and print enabled/disabled,
  ratio, logical page/OOB/erase/capacity, stripes, used pages, and tail.

- [ ] **Step 7: Set the default build profile and run focused tests.**

  Add to `configs/linux/mtd.fragment`:

  ```text
  CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID=y
  CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID_DATA_PAGES=4
  ```

  Run:

  ```sh
  ./tests/test_q3n_ecc.sh
  ./tests/test_q3n_legacy.sh
  ./tests/test_q3n_flash.sh
  ./tests/test_q3n_page.sh
  ./scripts/smoke-test.sh
  ```

- [ ] **Step 8: Commit Task 3.**

  ```sh
  git add linux/drivers/mtd/nand/raw/qemu_3dnand_ecc.* \
    linux/drivers/mtd/nand/raw/qemu_3dnand_controller.c \
    linux/drivers/mtd/nand/raw/qemu_3dnand_init.c \
    tests/test_q3n_ecc.c tests/test_q3n_legacy.c \
    tests/test_q3n_nand_core_contract.sh configs/linux/mtd.fragment
  git commit -m "mtd: route Q3N NAND Core I/O through page profiles"
  ```

---

## Task 4: Verify all profiles, exact-geometry patches, guest behavior, and documentation

**Files:**

- Modify: `tests/test_linux_patches.sh`
- Create: `tests/test_q3n_page_raid_config.sh`
- Modify: `scripts/smoke-test.sh`
- Modify: `docs/superpowers/specs/2026-07-28-q3n-nand-core-ecc-read-retry-design.md`
- Modify: `docs/superpowers/specs/2026-07-29-q3n-configurable-page-raid-logical-page-design.md`
- Modify: `docs/superpowers/plans/2026-07-29-q3n-configurable-page-raid-implementation.md`

- [x] **Step 1: Add failing configuration/object-composition tests.**

  Exercise copied overlay builds or Makefile evaluation for:

  ```text
  PAGE_RAID=n       -> page.o + layout.o, no page_raid.o
  PAGE_RAID=y, N=2  -> page.o + layout.o + page_raid.o
  PAGE_RAID=y, N=4  -> page.o + layout.o + page_raid.o
  PAGE_RAID=y, N=8  -> page.o + layout.o + page_raid.o
  ```

  Reject `N=3` through the runtime layout test even though Kconfig range is
  2..8. Run the new script before final wiring and confirm RED.

- [x] **Step 2: Extend exact-geometry patch behavior tests.**

  Reuse the patched Linux helper harness with literal logical
  writesize/pages-per-block/erasesize combinations:

  ```text
  32768 / 533 / 17465344
  65536 / 320 / 20971520
  131072 / 177 / 23199744
  ```

  Verify first/last page, cross-block page, last block, erase alignment, and
  416-byte BBT for each. The generic exact-geometry patches should pass
  without adding a profile-specific Linux source patch; if a real helper
  defect is exposed, update the tracked patch file and its RED/GREEN test.

- [x] **Step 3: Run host and Linux build verification for disabled and 4:1.**

  Run:

  ```sh
  ./scripts/smoke-test.sh
  ./scripts/build-kernel.sh
  Q3N_REQUIRE_KERNEL_BUILD=1 ./tests/test_q3n_nand_core_contract.sh
  ```

  Build a second kernel/module configuration with Page RAID disabled and
  verify its compiled contract has no RAID object/symbol.

- [x] **Step 4: Build-check 2:1 and 8:1.**

  Reconfigure the isolated kernel output to `N=2`, build
  `drivers/mtd/nand/raw/`, then repeat with `N=8`. Record the exact commands
  and successful module paths in the plan execution-status table.

- [x] **Step 5: Run guest integration with the default 4:1 profile.**

  Run the existing QEMU smoke/persistence workflow. Verify in guest:

  ```text
  writesize = 65536
  oobsize = 4096
  erasesize = 20971520
  size = 34896609280
  eraseblocks = 1664
  ```

  Exercise page read/write, OOB, erase, markbad, BBT rescan, persistence,
  and cross-logical-block I/O. Confirm dmesg prints `4 data + 1 parity`,
  `stripes/block=320`, and `tail=0`.

- [x] **Step 6: Run an 8:1 geometry/visibility guest boot.**

  Boot the `N=8` module/kernel and verify:

  ```text
  writesize = 131072
  oobsize = 8192
  erasesize = 23199744
  size = 38604374016
  stripes/block = 177
  tail pages/block = 7
  ```

  Confirm the tail is absent from MTD addressing and a complete logical
  block erase succeeds.

- [x] **Step 7: Update documents with implementation evidence.**

  Mark implemented items as complete, retain explicit exclusions for
  parity metadata and power-fail atomicity, and record host, kernel, QEMU,
  guest, BBT, and persistence evidence. Do not claim fault-injection cases
  that cannot be exercised in guest.

- [x] **Step 8: Run final verification.**

  ```sh
  git diff --check
  ./scripts/smoke-test.sh
  ./scripts/build-kernel.sh
  Q3N_REQUIRE_KERNEL_BUILD=1 ./tests/test_q3n_nand_core_contract.sh
  ```

  Also run the successful guest commands from Steps 5–6 and inspect
  `git status` for generated artifacts.

- [x] **Step 9: Commit Task 4.**

  ```sh
  git add tests/test_linux_patches.sh tests/test_q3n_page_raid_config.sh \
    scripts/smoke-test.sh docs/superpowers/specs \
    docs/superpowers/plans/2026-07-29-q3n-configurable-page-raid-implementation.md
  git commit -m "test: verify configurable Q3N page RAID profiles"
  ```

## Completion Criteria

1. RAID enable and data-page ratio are separate Kconfig settings.
2. Disabled mode preserves the current 16 KiB/1 KiB/25 MiB identity path.
3. 2:1, 4:1, and 8:1 report the exact logical geometries in Task 1.
4. Every logical page maps to `N` data pages plus the immediately following parity page without crossing a block.
5. Linux driver XORs parity and programs `D0..DN-1,P` synchronously.
6. `q3n_hw_*` and QEMU remain physical-page implementations.
7. Raw write maintains parity; raw read never performs recovery.
8. NAND Core read retry runs before exactly-one-data-page recovery.
9. OOB, BBM, bad-block callbacks, and RAM BBT retain the documented ownership and mapping.
10. No parity metadata, rollback, power-fail transaction, async cache, scheduler, or QEMU parity state is introduced.
11. A physical data or parity main buffer containing only `0xff` does not
    issue a main-data program; non-erased OOB is still preserved through an
    OOB-only program.
12. Host tests, exact-geometry patch tests, all four module configurations, default guest persistence, and 8:1 guest geometry pass.
13. The implementation, overall design, detailed design, plan, branch, and GitHub state agree.
