# Q3N Fixed pSLC Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the QEMU q3n device and Linux driver expose the existing fixed pSLC layout as SLC to NAND core, then prove RAID1 can mount UBI/UBIFS.

**Architecture:** QEMU keeps the existing physical READID and geometry but always advertises a new `Q3N_CAP_PSEUDO_SLC` capability. The Linux controller validates that capability and changes only the NAND memory organization's runtime `bits_per_cell` during `attach_chip()`, before `nand_scan_tail()` initializes MTD. RAID1 receives a reduced-capacity end-to-end UBI/UBIFS smoke test; RAID5 remains a raw-MTD profile.

**Tech Stack:** QEMU 11.0.2 device model in C, Linux 7.0.12 raw NAND/MTD driver in C, KUnit, POSIX shell, BusyBox initramfs, mtd-utils, UBI, UBIFS.

**Spec:** `docs/superpowers/specs/2026-08-30-multiplane-page-raid1-raid5-design.md`

## Global Constraints

- The physical NAND READID remains `9c d7 98 a6 51 33 4e 44`; pSLC and TLC use the same ID.
- The driver supports only fixed pSLC operation; there is no TLC path and no `pseudo-slc=off` switch.
- Existing pSLC geometry remains 16 KiB page, 1664 B physical OOB, 128 B logical OOB, 1600 pages/block, 247 blocks/plane, and 2 die × 4 plane.
- RAID1 logical writesize remains 16 KiB; RAID5 remains 48 KiB and is not claimed as UBI/UBIFS-compatible.
- Do not modify UBI, UBIFS, or generic NAND core to bypass MLC checks.
- Continue using `nand_scan_with_ids()`, NAND-core BBT creation, and the existing `ecc.*`/legacy callback architecture.
- New or changed behavior follows test-first red-green-refactor cycles.

## Review Focus

- A QEMU/driver ABI mismatch in the new capability bit must fail probe instead of silently registering TLC MTD.
- The physical READID must remain byte-for-byte unchanged while operational `bits_per_cell` becomes 1.
- The pSLC override must happen in `attach_chip()`, before `nand_scan_tail()`, so MTD becomes `MTD_NANDFLASH`.
- Reduced block-pool input must reject zero, non-numeric, and pool-overflow values before QEMU launch.
- The UBIFS smoke must never erase the default full-capacity image and must cleanly unmount/detach on success or failure.

---

### Task 1: Add the fixed pSLC controller capability

**Files:**
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand.h`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: existing `Q3N_REG_CAP` register and unchanged `q3n_default_nand_id`.
- Produces: shared ABI constant `Q3N_CAP_PSEUDO_SLC = BIT(4)`; QEMU always includes it in `Q3N_REG_CAP`.

- [ ] **Step 1: Add failing structural assertions**

Add assertions to `tests/test_scripts.sh`:

```sh
for symbol in Q3N_CAP_PSEUDO_SLC; do
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h "$symbol"
  assert_contains qemu/include/hw/mtd/q3n-nand.h "$symbol"
done
assert_function_contains qemu/hw/mtd/q3n-nand.c q3n_mmio_read \
  'Q3N_CAP_PSEUDO_SLC'
assert_contains qemu/hw/mtd/q3n-nand.c \
  '0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44'
```

- [ ] **Step 2: Run the focused test and verify RED**

Run: `./tests/test_scripts.sh`

Expected: FAIL because `Q3N_CAP_PSEUDO_SLC` is absent.

- [ ] **Step 3: Implement the shared capability**

Add the same ABI bit to both headers:

```c
#define Q3N_CAP_PSEUDO_SLC            BIT(4)
```

Use `(1U << 4)` in the QEMU header if that header does not include Linux `BIT()`. Extend only the `Q3N_REG_CAP` return value:

```c
return Q3N_CAP_BASIC_FLASH | Q3N_CAP_PERSISTENT_MEDIA |
       Q3N_CAP_BAD_BLOCK_MARKER | Q3N_CAP_MULTIPLANE |
       Q3N_CAP_PSEUDO_SLC;
```

Do not change `q3n_default_nand_id` or geometry constants.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run: `./tests/test_scripts.sh`

Expected: PASS.

- [ ] **Step 5: Commit the ABI change**

```bash
git add tests/test_scripts.sh qemu/include/hw/mtd/q3n-nand.h \
  qemu/hw/mtd/q3n-nand.c linux/drivers/mtd/nand/raw/qemu_3dnand.h
git commit -m "feat: advertise fixed q3n pseudo-SLC mode"
```

### Task 2: Apply pSLC semantics during NAND controller attach

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_device.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_nand.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: `q3n->cap`, `q3n->chip.base`, and `Q3N_CAP_PSEUDO_SLC`.
- Produces: `int q3n_device_apply_pslc(struct qemu_3dnand *q3n)`, which returns `-ENODEV` without the capability and otherwise sets `nanddev_get_memorg(&q3n->chip.base)->bits_per_cell = 1`.

- [ ] **Step 1: Add failing KUnit tests**

Add two tests to `qemu_3dnand_kunit.c` and register them in the device suite:

```c
static void q3n_device_pslc_requires_capability(struct kunit *test)
{
	struct qemu_3dnand *q3n = kunit_kzalloc(test, sizeof(*q3n),
					       GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, q3n);
	nanddev_get_memorg(&q3n->chip.base)->bits_per_cell = 3;
	KUNIT_EXPECT_EQ(test, q3n_device_apply_pslc(q3n), -ENODEV);
	KUNIT_EXPECT_EQ(test,
		nanddev_bits_per_cell(&q3n->chip.base), 3U);
}

static void q3n_device_pslc_sets_runtime_cell_type(struct kunit *test)
{
	struct qemu_3dnand *q3n = kunit_kzalloc(test, sizeof(*q3n),
					       GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, q3n);
	q3n->cap = Q3N_CAP_PSEUDO_SLC;
	nanddev_get_memorg(&q3n->chip.base)->bits_per_cell = 3;
	KUNIT_ASSERT_EQ(test, q3n_device_apply_pslc(q3n), 0);
	KUNIT_EXPECT_TRUE(test, nand_is_slc(&q3n->chip));
}
```

Add a structural assertion that `qemu_3dnand_attach_chip` calls `q3n_device_apply_pslc`.

- [ ] **Step 2: Run KUnit structure/build tests and verify RED**

Run:

```bash
./tests/test_scripts.sh
./scripts/shell.sh ./scripts/build-kernel.sh
```

Expected: the structural test fails and the kernel build fails because `q3n_device_apply_pslc` is undefined.

- [ ] **Step 3: Implement the pSLC helper and attach hook**

Declare the helper in `qemu_3dnand_internal.h` and implement it in `qemu_3dnand_device.c`:

```c
int q3n_device_apply_pslc(struct qemu_3dnand *q3n)
{
	struct nand_memory_organization *memorg;

	if (!q3n || !(q3n->cap & Q3N_CAP_PSEUDO_SLC))
		return -ENODEV;
	memorg = nanddev_get_memorg(&q3n->chip.base);
	memorg->bits_per_cell = 1;
	return 0;
}
```

Add `Q3N_CAP_PSEUDO_SLC` to the YMTC descriptor's `required_caps`. At the beginning of `qemu_3dnand_attach_chip()`, call the helper and return its error before installing ECC callbacks.

- [ ] **Step 4: Add post-scan validation**

After `nand_scan_with_ids()` succeeds and before MTD registration, reject inconsistent runtime state:

```c
if (!nand_is_slc(&q3n->chip) || q3n->mtd->type != MTD_NANDFLASH) {
	ret = -EINVAL;
	goto err_cleanup;
}
```

Keep the original physical ID in `q3n_device_build_scan_id()`; do not clear the ID's TLC cell bits.

- [ ] **Step 5: Run tests and verify GREEN**

Run:

```bash
./tests/test_scripts.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-kunit-smoke.sh
./scripts/q3n-raid1-smoke.sh
./scripts/q3n-raid5-smoke.sh
```

Expected: all commands PASS; kernel log may identify the physical ID as TLC during ident, but registered MTD is SLC after attach.

- [ ] **Step 6: Commit the Linux pSLC behavior**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_device.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_nand.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_internal.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c tests/test_scripts.sh
git commit -m "feat: expose q3n runtime geometry as pseudo-SLC"
```

### Task 3: Add a safe reduced-capacity QEMU launch option

**Files:**
- Modify: `scripts/run-qemu.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: QEMU's existing `data-blocks-per-plane` property.
- Produces: host option `--data-blocks-per-plane N`; default remains 208.

- [ ] **Step 1: Add failing script assertions and argument tests**

Extend `tests/test_scripts.sh` to assert the option, property, numeric validation, and default:

```sh
assert_contains scripts/run-qemu.sh '--data-blocks-per-plane'
assert_contains scripts/run-qemu.sh 'data-blocks-per-plane='
assert_contains scripts/run-qemu.sh 'data_blocks_per_plane=208'
assert_contains scripts/run-qemu.sh '\*\[\!0-9\]\*'
```

Add a shell invocation with `--data-blocks-per-plane invalid` and assert it exits nonzero before trying to locate QEMU.

- [ ] **Step 2: Run the script test and verify RED**

Run: `./tests/test_scripts.sh`

Expected: FAIL because the new host option is absent.

- [ ] **Step 3: Implement validated device argument construction**

Initialize `data_blocks_per_plane=208`, parse `--data-blocks-per-plane N`, reject zero/non-numeric values, and reject values above 208 for the smoke-test interface. Build:

```sh
q3n_device="q3n-nand-pci,drive=q3n-media,data-blocks-per-plane=$data_blocks_per_plane"
```

Pass `-device "$q3n_device"`. Keep all existing options and defaults unchanged.

- [ ] **Step 4: Run tests and verify GREEN**

Run:

```bash
./tests/test_scripts.sh
./scripts/run-qemu.sh --help
```

Expected: PASS; help lists the new option and states that it is intended for reduced-capacity tests.

- [ ] **Step 5: Commit the launch option**

```bash
git add scripts/run-qemu.sh tests/test_scripts.sh
git commit -m "test: support reduced q3n block pools"
```

### Task 4: Prove RAID1 pSLC with UBI/UBIFS

**Files:**
- Create: `scripts/q3n-ubifs-smoke.sh`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `rootfs/init`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: `--data-blocks-per-plane 8`, guest `raid_level=1`, compression modules, UBI and UBIFS.
- Produces: guest function `mtd_q3n_ubifs_smoke()`, boot selector `MTD_SMOKE=q3n-ubifs-smoke`, and success marker `q3n pSLC RAID1 UBIFS smoke passed`.

- [ ] **Step 1: Add failing structure tests**

Add assertions for the new executable, boot selector, guest function, pSLC/MTD checks, and success marker:

```sh
assert_file scripts/q3n-ubifs-smoke.sh
assert_executable scripts/q3n-ubifs-smoke.sh
assert_contains rootfs/init 'q3n-ubifs-smoke'
assert_contains rootfs/profile.d/mtd.sh 'mtd_q3n_ubifs_smoke'
assert_contains rootfs/profile.d/mtd.sh '/type'
assert_contains rootfs/profile.d/mtd.sh 'grep -qx nand'
assert_contains rootfs/profile.d/mtd.sh 'q3n pSLC RAID1 UBIFS smoke passed'
```

- [ ] **Step 2: Run the structure test and verify RED**

Run: `./tests/test_scripts.sh`

Expected: FAIL because the host wrapper and guest function do not exist.

- [ ] **Step 3: Implement the guest smoke workflow**

Implement `mtd_q3n_ubifs_smoke()` with one cleanup trap/helper. It must:

```sh
modprobe qemu_3dnand raid_level=1
modprobe zlib_deflate
modprobe deflate
modprobe zstd_compress
modprobe zstd
modprobe ubi
modprobe ubifs
```

Then locate only the `qemu-3dnand` MTD, verify writesize is 16384, and
require `/sys/class/mtd/mtd${mtd_num}/type` to equal `nand`:

```sh
grep -qx nand "/sys/class/mtd/mtd${mtd_num}/type" || return 1
```

Run `ubiformat -q -y`, attach UBI, create a maximum-size `rootfs` volume,
mount UBIFS, write and read a deterministic file, sync, unmount, detach, and
print the success marker. Cleanup must unmount and detach on every failure
path.

- [ ] **Step 4: Implement the host wrapper**

Create `scripts/q3n-ubifs-smoke.sh` following the existing smoke wrappers:

```sh
if ! "$repo_root/scripts/run-qemu.sh" --fresh-nand \
     --data-blocks-per-plane 8 \
     --append "MTD_SMOKE=q3n-ubifs-smoke" >"$log" 2>&1; then
  cat "$log"
  die "q3n pSLC RAID1 UBIFS QEMU failed"
fi
```

Require both the q3n UBIFS marker and generic MTD success marker.

- [ ] **Step 5: Rebuild QEMU/kernel/rootfs and verify GREEN end to end**

Run:

```bash
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-ubifs-smoke.sh
```

Expected: UBI attaches without the MLC refusal, UBIFS mounts, the file round-trip succeeds, cleanup completes, and QEMU powers off.

- [ ] **Step 6: Commit the end-to-end test**

```bash
git add scripts/q3n-ubifs-smoke.sh rootfs/profile.d/mtd.sh \
  rootfs/init tests/test_scripts.sh
git commit -m "test: mount UBIFS on q3n pseudo-SLC RAID1"
```

### Task 5: Document pSLC mounting and run the full regression suite

**Files:**
- Modify: `README.md`
- Modify: `qemu/README.md`
- Modify: `docs/qemu-3dnand-register-reference.md`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: fixed pSLC capability and `q3n-ubifs-smoke.sh`.
- Produces: user-facing mounting commands that do not recommend a full-capacity `flash_erase -q ... 0 0`.

- [ ] **Step 1: Add failing documentation assertions**

Assert that the README files name `Q3N_CAP_PSEUDO_SLC`, explain same-ID pSLC operation, list `q3n-ubifs-smoke.sh`, and warn against whole-device erase of the default geometry.

- [ ] **Step 2: Run the documentation test and verify RED**

Run: `./tests/test_scripts.sh`

Expected: FAIL because the documentation does not yet contain the pSLC workflow.

- [ ] **Step 3: Update documentation**

Document:

- fixed pSLC-only support and unchanged physical READID;
- current pSLC physical and RAID logical geometry;
- capability → `attach_chip()` → SLC MTD flow;
- automatic RAID1 UBIFS command `./scripts/q3n-ubifs-smoke.sh`;
- manual compression-module, UBI, and UBIFS load order;
- why RAID5 cannot use unmodified UBI/UBIFS;
- why `flash_erase -q "$MTD_DEV" 0 0` is inappropriate for the default large QEMU image.

- [ ] **Step 4: Run complete verification**

Run:

```bash
./scripts/smoke-test.sh
./scripts/q3n-kunit-smoke.sh
./scripts/q3n-raid1-smoke.sh
./scripts/q3n-raid5-smoke.sh
./scripts/q3n-persistence-smoke.sh
./scripts/q3n-ubifs-smoke.sh
git diff --check
```

Expected: every command PASS and no whitespace errors.

- [ ] **Step 5: Commit documentation**

```bash
git add README.md qemu/README.md docs/qemu-3dnand-register-reference.md \
  tests/test_scripts.sh
git commit -m "docs: describe q3n pseudo-SLC UBIFS workflow"
```

- [ ] **Step 6: Push the completed branch**

```bash
git push origin codex/multiplane-page-raid1-raid5
```

Expected: the remote branch advances and `git status --short --branch` is clean.
