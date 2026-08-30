# Multi-plane Page-RAID1/RAID5 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 q3n QEMU NAND 模型和 Linux direct MTD 驱动中实现同一 die 内的双 plane RAID1 与四 plane rotating-parity RAID5，并以 OOB manifest 提供可恢复的两阶段提交。

**Architecture:** QEMU 增加与 RAID 无关的四 slot multi-plane ABI，并保留逐 slot program/read/erase/ECC 结果；Linux 用 profile mapper 将 logical eraseblock 映射到同一 die 的 plane pair 或四 plane group。所有 main 成员成功后才以 multi-plane OOB program 发布 v2 manifest；RAID1 读取镜像回退，RAID5 读取其余成员 XOR 重建。

**Tech Stack:** QEMU 11.x C、Linux 7.0.12 MTD/PCI 驱动、KUnit、POSIX shell、BusyBox/mtd-utils、QEMU guest smoke tests。

**Spec:** `docs/superpowers/specs/2026-08-30-multiplane-page-raid1-raid5-design.md`

## Global Constraints

- 基线提交为 `6a687d8`，实现分支为 `codex/multiplane-page-raid1-raid5`。
- 固定物理拓扑为 2 die × 4 plane；冗余成员不得跨 die。
- RAID1 固定为 `plane0+plane1` 与 `plane2+plane3` 两组镜像，MTD writesize 为 16 KiB。
- RAID5 固定为每 die `3D+1P`，parity plane 为 `stripe_id % 4`，MTD writesize 为 48 KiB。
- `raid_level=1|5` 在驱动加载时选择，默认 5，运行期不可切换。
- OOB byte 0 保留为 BBM；v2 manifest 从 byte 1 开始，不能占用 LDPC 区。
- Main program 与 OOB commit 必须分成两个 multi-plane 命令；任一 main slot 失败不得写 manifest。
- 不实现 FTL、GC、磨损均衡、坏块替换、后台重建或双成员恢复。
- 不修改 MTD core、UBI 或 UBIFS；RAID5 不承诺兼容 16 KiB UBI 小写。
- 现有未跟踪 `.vscode/` 不读取、不修改、不提交。

---

## File Structure

```text
qemu/include/hw/mtd/q3n-nand.h             # QEMU multi-plane ABI constants
qemu/hw/mtd/q3n-nand.c                     # four-slot staging and execution
tests/test_q3n_controller.c                 # extracted QEMU helper unit tests
tests/test_scripts.sh                       # ABI/overlay structural gates

linux/drivers/mtd/nand/raw/qemu_3dnand.h   # Linux copy of controller ABI
linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h
                                             # RAID profiles, layouts, manifest
linux/drivers/mtd/nand/raw/qemu_3dnand_map.c # pure RAID1/RAID5 mapper
linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c
                                             # manifest, CRC, XOR, validation
linux/drivers/mtd/nand/raw/qemu_3dnand_mp.c  # Linux MMIO multi-plane transport
linux/drivers/mtd/nand/raw/qemu_3dnand_main.c
                                             # MTD lifecycle and profile I/O
linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c
                                             # mapper/manifest/recovery KUnit
linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand
scripts/apply-linux-overlay.sh              # install new compilation unit

rootfs/profile.d/mtd.sh                     # guest RAID1/RAID5 acceptance
rootfs/init                                 # MTD_SMOKE dispatch
scripts/q3n-kunit-smoke.sh                  # host KUnit wrapper
scripts/q3n-raid1-smoke.sh                  # host RAID1 wrapper
scripts/q3n-raid5-smoke.sh                  # host RAID5 wrapper
README.md                                   # user workflow and limitations
qemu/README.md                              # ABI and model behavior
```

Delete the old scheduler source and serial host wrapper, remove them from `Makefile.qemu_3dnand`, `apply-linux-overlay.sh`, runtime state, KUnit cases, rootfs dispatch, and structural tests. The synchronous multi-plane profiles must not allocate `q3n-parity` workqueues or enqueue P0/P1/P2 requests.

---

### Task 1: Define and Unit-Test the Multi-plane ABI

**Files:**
- Modify: `tests/test_q3n_controller.c`
- Modify: `tests/test_scripts.sh`
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand.h`
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `docs/qemu-3dnand-register-reference.md`

**Interfaces:**
- Consumes: existing physical address convention and PIO data window.
- Produces: identical QEMU/Linux constants, `Q3NMultiPlaneDesc`, and `q3n_mp_validate_desc()` for later controller execution.

- [ ] **Step 1: Add failing host tests for descriptor validation**

Add the following constants before the extracted helper include in `tests/test_q3n_controller.c`. Add `Q3NMultiPlaneDesc` and `q3n_mp_validate_desc()` inside the extracted controller-helper block, then add this test after the include and call it from `main()`:

```c
#define Q3N_DIES                 2U
#define Q3N_PLANES_PER_DIE       4U
#define Q3N_BLOCKS_PER_PLANE     247U
#define Q3N_PAGES_PER_BLOCK      1600U

static void test_multiplane_descriptor_validation(void)
{
    Q3NMultiPlaneDesc d = {
        .die = 0,
        .plane_mask = 0x0f,
        .addr = {
            3ULL * Q3N_PAGE_SIZE,
            ((uint64_t)Q3N_BLOCKS_PER_PLANE * Q3N_PAGES_PER_BLOCK + 3) * Q3N_PAGE_SIZE,
            ((uint64_t)2 * Q3N_BLOCKS_PER_PLANE * Q3N_PAGES_PER_BLOCK + 3) * Q3N_PAGE_SIZE,
            ((uint64_t)3 * Q3N_BLOCKS_PER_PLANE * Q3N_PAGES_PER_BLOCK + 3) * Q3N_PAGE_SIZE,
        },
        .staged = { Q3N_PAGE_SIZE, Q3N_PAGE_SIZE,
                    Q3N_PAGE_SIZE, Q3N_PAGE_SIZE },
    };

    assert(q3n_mp_validate_desc(&d, Q3N_PAGE_SIZE, false));
    d.addr[3] += Q3N_PAGE_SIZE;
    assert(!q3n_mp_validate_desc(&d, Q3N_PAGE_SIZE, false));
    d.addr[3] -= Q3N_PAGE_SIZE;
    d.die = 1;
    assert(!q3n_mp_validate_desc(&d, Q3N_PAGE_SIZE, false));
    d.die = 0;
    d.plane_mask = 0;
    assert(!q3n_mp_validate_desc(&d, Q3N_PAGE_SIZE, false));
}
```

Add structural assertions for every new command/register/capability in both headers.

- [ ] **Step 2: Run the host test and verify RED**

Run: `sh tests/test_scripts.sh`

Expected: compilation fails because `q3n_mp_validate_desc` and the multi-plane ABI symbols do not exist.

- [ ] **Step 3: Add exact ABI constants to both headers**

Use the same values in both headers:

```c
#define Q3N_CAP_MULTIPLANE             BIT(3) /* QEMU uses (1U << 3) */
#define Q3N_REG_MP_DIE                 0x00bc
#define Q3N_REG_MP_PLANE_MASK          0x00c0
#define Q3N_REG_MP_SLOT                0x00c4
#define Q3N_REG_MP_ADDR_LO             0x00c8
#define Q3N_REG_MP_ADDR_HI             0x00cc
#define Q3N_REG_MP_SUCCESS_MASK        0x00d0
#define Q3N_REG_MP_FAILURE_MASK        0x00d4
#define Q3N_REG_MP_ECC_STATUS          0x00d8
#define Q3N_REG_MP_ECC_MAX_BITFLIPS    0x00dc
#define Q3N_REG_MP_ECC_CORRECTED_BITS  0x00e0
#define Q3N_REG_MP_ECC_FAILED_STEP     0x00e4
#define Q3N_REG_STAT_MP_COMMANDS       0x00e8
#define Q3N_REG_STAT_MP_SLOT_FAILURES  0x00ec

#define Q3N_CMD_MP_READ_PAGE           9
#define Q3N_CMD_MP_PROGRAM_PAGE        10
#define Q3N_CMD_MP_READ_PAGE_OOB       11
#define Q3N_CMD_MP_PROGRAM_PAGE_OOB    12
#define Q3N_CMD_MP_ERASE_BLOCK         13
```

Document selected-slot readback semantics: `MP_SLOT` selects address, staging cursor, data window and ECC-result view; success/failure masks describe the last completed multi-plane command.

- [ ] **Step 4: Implement the pure validator in the extracted helper block**

Add a helper that decodes `addr / Q3N_PAGE_SIZE` into lane, block-in-plane and page. `q3n_mp_validate_desc(desc, required_staged, erase)` returns true only when: die < 2, mask is nonzero and within `0x0f`, every enabled slot decodes to `lane == die * 4 + slot`, every enabled member has the same block-in-plane/page, staging equals `required_staged` for program, and erase addresses select page zero.

- [ ] **Step 5: Run host tests and verify GREEN**

Run: `./scripts/smoke-test.sh`

Expected: `ok: q3n controller OOB and LDPC behavior verified`, followed by `ok: smoke test passed`.

- [ ] **Step 6: Commit**

```bash
git add tests/test_q3n_controller.c tests/test_scripts.sh \
  qemu/include/hw/mtd/q3n-nand.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand.h \
  qemu/hw/mtd/q3n-nand.c docs/qemu-3dnand-register-reference.md
git commit -m "feat: define q3n multiplane controller ABI"
```

---

### Task 2: Implement Four-slot QEMU Execution

**Files:**
- Modify: `tests/test_q3n_controller.c`
- Modify: `tests/test_scripts.sh`
- Modify: `qemu/hw/mtd/q3n-nand.c`

**Interfaces:**
- Consumes: Task 1 ABI and `q3n_mp_validate_desc()`.
- Produces: functional multi-plane main/OOB read/program/erase with per-slot status and ECC.

- [ ] **Step 1: Add failing staging/status tests**

Add pure helper tests proving that selecting/resetting slot 2 does not clear slot 0, and result accounting produces `success=0x0d`, `failure=0x02` for a four-slot command whose plane 1 fails. Add structural gates requiring all five command handlers in `q3n_execute_cmd()` and `Q3N_CAP_MULTIPLANE` in `Q3N_REG_CAP`.

- [ ] **Step 2: Run and verify RED**

Run: `sh tests/test_scripts.sh`

Expected: missing multi-plane staging helpers/handlers.

- [ ] **Step 3: Add controller state**

Add:

```c
typedef struct Q3NMultiPlaneSlot {
    uint64_t addr;
    uint8_t data[Q3N_PAGE_SIZE + Q3N_LOGICAL_OOB_SIZE];
    uint32_t data_pos;
    uint32_t data_count;
    Q3NEccResult ecc;
} Q3NMultiPlaneSlot;

uint32_t mp_die;
uint32_t mp_plane_mask;
uint32_t mp_slot;
uint32_t mp_success_mask;
uint32_t mp_failure_mask;
Q3NMultiPlaneSlot mp[Q3N_PLANES_PER_DIE];
```

Add `multiplane_commands` and `multiplane_slot_failures` to `Q3NStats`.

- [ ] **Step 4: Implement selected-slot MMIO behavior**

Writing `MP_SLOT` accepts only 0..3 and selects the MP data window. `MP_ADDR_LO/HI`, `LEN`, `OOB_LEN`, and the data window then operate on `mp[mp_slot]`. Writing either legacy `ADDR_LO/HI` reselects the single-page window, so existing commands retain `s->addr` and `s->data_buf` behavior. Reset clears every MP cursor, address, result mask and ECC result and reselects the single-page window.

- [ ] **Step 5: Implement command execution**

For each command, build `Q3NMultiPlaneDesc`, validate all slots before media mutation, clear result masks, execute enabled planes in ascending order, and record each outcome. Refactor program fault matching to:

```c
static bool q3n_should_fail_program(Q3NNandState *s, uint64_t addr)
{
    if (!s->fail_next_program || addr != s->fail_program_addr) {
        return false;
    }
    q3n_disarm_program_fault(s);
    s->stats.faults_injected++;
    return true;
}
```

Use it for both main and OOB program so a manifest slot can fail independently. A descriptor validation error sets all enabled bits in failure mask without media writes. A runtime slot failure preserves successes in other slots. Any failure sets controller ERROR, but all masks/results remain readable until the next command.

- [ ] **Step 6: Verify unit and build gates**

Run:

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
```

Expected: both exit 0; QEMU compile has no warnings; host helper tests pass.

- [ ] **Step 7: Commit**

```bash
git add tests/test_q3n_controller.c tests/test_scripts.sh qemu/hw/mtd/q3n-nand.c
git commit -m "feat: execute q3n multiplane NAND commands"
```

---

### Task 3: Replace Serial Mapping with RAID Profiles

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_map.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `rootfs/init`
- Create: `scripts/q3n-kunit-smoke.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: physical 2×4 geometry.
- Produces: `q3n_map_raid1_page()`, `q3n_map_raid5_stripe()`, `q3n_raid_geometry_values()`.

- [ ] **Step 1: Replace old mapper tests with failing profile tests**

Use this public shape:

```c
enum q3n_raid_level { Q3N_RAID1 = 1, Q3N_RAID5 = 5 };

struct q3n_geometry {
    u32 page_size;
    u32 pages_per_block;
    u32 blocks_per_plane;
    u32 data_blocks_per_plane;
    u8 dies;
    u8 planes_per_die;
    enum q3n_raid_level raid_level;
};

struct q3n_raid_group {
    u64 stripe_id;
    u32 block_in_plane;
    u32 page;
    u8 die;
    u8 member_mask;
    u8 parity_plane;
    u8 data_pages;
    struct q3n_phys_addr member[4];
};
```

KUnit cases must assert RAID1 `leb=0..3` maps to `(die,pair)=(0,0),(0,1),(1,0),(1,1)`, `leb=4` maps to die 0/pair 0/block 1; RAID5 `leb=0,1,2` maps to die 0/block 0, die 1/block 0, die 0/block 1. Four consecutive page rows must produce parity planes 0,1,2,3.

- [ ] **Step 2: Build and verify RED**

Run: `./scripts/shell.sh ./scripts/build-kernel.sh`

Expected: KUnit compilation fails on missing profile types/functions.

- [ ] **Step 3: Implement mapping and geometry**

Implement checked integer arithmetic and these formulas exactly:

```text
physical block = (die * 4 + plane) * blocks_per_plane + block_in_plane
RAID1 set       = leb % 4; block_in_plane = leb / 4
RAID5 die       = leb % 2; block_in_plane = leb / 2
stripe_id       = leb * pages_per_block + page
parity_plane    = stripe_id % 4
```

`q3n_raid_geometry_values()` returns `(writesize, erasesize, size)` as `(page, pages_per_block*page, 4*data_blocks_per_plane*erasesize)` for RAID1 and `(3*page, pages_per_block*3*page, 2*data_blocks_per_plane*erasesize)` for RAID5. Reject any overflow, unsupported topology, out-of-range LEB/page/data slot, or RAID level other than 1/5.

- [ ] **Step 4: Add and run a deterministic KUnit guest wrapper**

Add `mtd_q3n_kunit_smoke()` which runs `modprobe qemu_3dnand_test`, captures the new kernel log, rejects `not ok` or `failed`, and prints `q3n KUnit smoke passed`. Dispatch `MTD_SMOKE=q3n-kunit-smoke` in `rootfs/init`. The host wrapper uses fresh NAND and requires both the KUnit marker and generic MTD success marker.

Run:

```bash
./scripts/shell.sh ./scripts/configure-kernel.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-kunit-smoke.sh
```

Expected: host wrapper exits 0, kernel log contains the qemu-3dnand KUnit suite with no failed case, and both required markers are present.

- [ ] **Step 5: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_map.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c \
  rootfs/profile.d/mtd.sh rootfs/init scripts/q3n-kunit-smoke.sh \
  tests/test_scripts.sh
git commit -m "feat: map q3n raid1 and raid5 plane groups"
```

---

### Task 4: Add Manifest v2 and Recovery Primitives

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`

**Interfaces:**
- Consumes: Task 3 `q3n_raid_group`.
- Produces: v2 manifest packing/validation, RAID1 copy selection, RAID5 XOR reconstruction.

- [ ] **Step 1: Write failing KUnit cases**

Define and test this exact packed format:

```c
#define Q3N_RAID_META_VERSION 2
#define Q3N_RAID_NO_PARITY    0xff

struct q3n_raid_manifest {
    __le16 magic;
    u8 version;
    u8 raid_level;
    u8 die;
    u8 member_bitmap;
    u8 parity_plane;
    u8 data_pages;
    __le64 stripe_id;
    __le32 generation;
    __le32 data_crc[3];
    __le32 parity_crc;
    __le32 header_crc;
} __packed;
```

Tests must cover: manifest begins at OOB byte 1 and leaves byte 0 unchanged; RAID1 requires one data CRC, a legal pair mask (`0x03` or `0x0c`) and no parity; RAID5 requires three CRCs, member mask `0x0f` and parity plane 0..3; header CRC corruption, zero generation, wrong die/physical group and conflicting copies return `-EBADMSG`; XOR recovery reconstructs D0, D1 and D2 and rejects missing source pointers.

- [ ] **Step 2: Verify RED**

Run: `./scripts/shell.sh ./scripts/build-kernel.sh`

Expected: old manifest fields/functions cannot satisfy the v2 tests.

- [ ] **Step 3: Implement minimal v2 helpers**

Expose:

```c
int q3n_build_manifest(const struct q3n_raid_group *group, u32 generation,
                       const u32 data_crc[3], u32 parity_crc,
                       struct q3n_raid_manifest *out);
int q3n_pack_manifest_oob(u8 oob[Q3N_LOGICAL_OOB_SIZE],
                          const struct q3n_raid_manifest *manifest);
int q3n_unpack_manifest_oob(const u8 oob[Q3N_LOGICAL_OOB_SIZE],
                            const struct q3n_raid_group *expected,
                            struct q3n_raid_manifest *out);
int q3n_raid5_recover(u8 *out, const u8 *parity,
                      const u8 *other0, const u8 *other1, size_t len);
```

Use `BUILD_BUG_ON(sizeof(struct q3n_raid_manifest) > Q3N_LOGICAL_OOB_SIZE - 1)`. Pack by filling OOB with `0xff`, preserving caller-supplied byte 0, and copying manifest at offset 1. CRC is `crc32_le(~0, bytes, sizeof(manifest))` with `header_crc=0` during calculation.

- [ ] **Step 4: Verify GREEN**

Run: `./scripts/shell.sh ./scripts/build-kernel.sh`, load the KUnit module, and confirm all new manifest/recovery cases pass.

- [ ] **Step 5: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c
git commit -m "feat: add q3n raid1 raid5 manifest v2"
```

---

### Task 5: Add the Linux Multi-plane Transport and Remove Runtime Scheduler Use

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_mp.c`
- Delete: `linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 1 QEMU ABI.
- Produces: `q3n_mp_read/program/read_oob/program_oob/erase()` returning per-slot results.

- [ ] **Step 1: Add failing structural and compile gates**

Require the new file in `tests/test_scripts.sh`, require `qemu_3dnand_mp.o` in the Makefile, and forbid `alloc_workqueue("q3n-parity"`, `q3n_sched_`, and `qemu_3dnand_parity_worker` in `qemu_3dnand_main.c`.

- [ ] **Step 2: Run and verify RED**

Run: `./scripts/smoke-test.sh`

Expected: missing `qemu_3dnand_mp.c` and still-active scheduler assertions fail.

- [ ] **Step 3: Define the transport interface**

Add:

```c
struct q3n_mp_io {
    void __iomem *regs;
    u32 page_size;
};

struct q3n_mp_result {
    u8 success_mask;
    u8 failure_mask;
    struct q3n_ecc_result ecc[4];
};

struct q3n_mp_buffers {
    u8 mask;
    u8 die;
    struct q3n_phys_addr addr[4];
    u8 *data[4];
};
```

Each helper stages enabled slots, issues one command, reads success/failure masks even when controller ERROR is set, and reads selected-slot ECC after read commands. It returns `0` only when the controller completed and `failure_mask==0`; callers may still inspect partial status on `-EIO`.

- [ ] **Step 4: Implement and integrate transport**

Move only MMIO multi-plane mechanics into `qemu_3dnand_mp.c`. Embed `struct q3n_mp_io mp` in the device and initialize it after BAR mapping. Keep single-page commands solely for probe/basic fault/status compatibility. Remove scheduler/workqueue/barrier fields, parity work allocation, old serial parity index, and scheduler-specific debugfs from active main code. Delete `qemu_3dnand_sched.c`, remove `qemu_3dnand_sched.o` from the module build, and remove its copy from overlay application.

- [ ] **Step 5: Verify host and kernel builds**

Run:

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/configure-kernel.sh
./scripts/shell.sh ./scripts/build-kernel.sh
```

Expected: all exit 0, and `qemu_3dnand.ko` contains `q3n_mp_program` and no unresolved scheduler symbol.

- [ ] **Step 6: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_mp.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
  linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
  scripts/apply-linux-overlay.sh tests/test_scripts.sh
git rm linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c
git commit -m "feat: add Linux q3n multiplane transport"
```

---

### Task 6: Implement RAID1 MTD I/O

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `rootfs/init`
- Create: `scripts/q3n-raid1-smoke.sh`
- Delete: `scripts/q3n-serial-smoke.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 3 mapper, Task 4 manifest, Task 5 transport.
- Produces: `raid_level=1` MTD with 16 KiB writes, two-phase mirror commits and fallback reads.

- [ ] **Step 1: Add failing guest acceptance path**

Add `mtd_q3n_raid1_smoke()` that loads `qemu_3dnand raid_level=1`, asserts sysfs writesize `16384`, erases one logical block, writes deterministic 16 KiB pages, reads them back, checks `raid_level=1`, `multiplane_commands`, `metadata_degraded`, `raid_recovered`, and `raid_failed`. Inject 41 bitflips into the selected primary physical member through a debugfs helper and assert read data matches plus recovery increments; inject both members and assert the read fails.

The host wrapper must run fresh NAND with `MTD_SMOKE=q3n-raid1-smoke`, require both `q3n raid1 smoke passed` and the generic MTD success marker, and print the full log on failure. Remove the old `mtd_q3n_serial_*` functions and init dispatch, then delete `scripts/q3n-serial-smoke.sh` and its structural assertions.

- [ ] **Step 2: Verify RED**

Run: `./scripts/smoke-test.sh`

Expected: missing RAID1 script/dispatch/debugfs symbols.

- [ ] **Step 3: Add profile parameter and MTD geometry**

Add:

```c
static unsigned int raid_level = Q3N_RAID5;
module_param(raid_level, uint, 0444);
MODULE_PARM_DESC(raid_level, "Page RAID level: 1 or 5");
```

Probe rejects values other than 1/5 and missing `Q3N_CAP_MULTIPLANE`. RAID1 registers `_read`, `_write`, `_erase`, `_sync`, `_block_isbad`, and `_block_markbad` directly. The RAID manifest area is driver-private: set `mtd->oobsize = 0`, `mtd->oobavail = 0`, leave `_read_oob/_write_oob` unset, and do not route main reads/writes through the old public OOB path. BBM and manifest still use controller OOB internally.

- [ ] **Step 4: Implement RAID1 two-phase write**

For each aligned logical page:

1. Map mirror pair.
2. Stage identical data in both enabled slots and call `q3n_mp_program()`.
3. Require `success_mask == member_mask`; otherwise return `-EIO` without OOB.
4. Build a v2 manifest with `data_crc[0]` and program identical OOB to both slots.
5. Require at least one OOB success; mark committed bitmap and increment `metadata_degraded` if success mask is not the full pair.
6. Set `retlen` only after commit evidence exists.

- [ ] **Step 5: Implement RAID1 read/fallback**

Use `(logical_page & 1)` to choose which mirror is read first. Load a valid committed manifest from either member, read/validate the preferred copy, and fall back to the peer on transport failure, LDPC uncorrectable, or CRC mismatch. A fallback success sets returned max bitflips to at least `mtd->bitflip_threshold`, increments `raid_recovered`, and does not increment standard failed. Both failures return `-EBADMSG`, increment standard failed exactly once and increment `raid_failed` exactly once.

- [ ] **Step 6: Run RAID1 acceptance**

Run:

```bash
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-raid1-smoke.sh
```

Expected: both RAID1 and generic markers present; single-copy recovery increments by one; double failure does not return data.

- [ ] **Step 7: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c \
  rootfs/profile.d/mtd.sh rootfs/init scripts/q3n-raid1-smoke.sh \
  tests/test_scripts.sh
git rm scripts/q3n-serial-smoke.sh
git commit -m "feat: add q3n multiplane page raid1"
```

---

### Task 7: Implement RAID5 MTD I/O

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `rootfs/init`
- Create: `scripts/q3n-raid5-smoke.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Tasks 3–5 and common MTD accounting from Task 6.
- Produces: default `raid_level=5` MTD with 48 KiB stripe writes, parity rotation and single-data recovery.

- [ ] **Step 1: Add failing RAID5 guest acceptance**

The guest test loads default RAID5, asserts writesize `49152`, writes four deterministic 48 KiB stripes, reads each 16 KiB data segment through normal MTD reads, and checks a debugfs `last_parity_plane` sequence of 0,1,2,3. It then injects 41 bitflips into each data role in separate fresh stripes, confirms exact reconstructed bytes and `-EUCLEAN` behavior, injects target+source failures and confirms `-EBADMSG`, and verifies a healthy target remains readable when parity alone fails.

- [ ] **Step 2: Verify RED**

Run: `./scripts/smoke-test.sh`

Expected: RAID5 wrapper/markers and driver parity-plane observability are missing.

- [ ] **Step 3: Implement RAID5 write**

Reject unaligned `to` or `len` not divisible by 49152. For each stripe, map D0/D1/D2 to ascending non-parity planes, calculate 16 KiB XOR parity, CRC all data and parity, issue a four-slot main program, require all four main successes, then program the identical v2 manifest to all four OOBs. At least one manifest success commits the stripe; partial manifest success increments `metadata_degraded`. Advance `retlen` by 49152 only after commit.

- [ ] **Step 4: Implement RAID5 read/recovery**

Map arbitrary read ranges into `(leb,page_row,data_slot,column)`. Healthy target reads use one physical page and compare against the corresponding manifest CRC. On failure, issue one MP read for parity plus the other two data members, accumulate their real ECC statistics, validate parity CRC, XOR into `raid_buf`, and compare target CRC. Recovery success forms `-EUCLEAN`; any required-source failure or CRC mismatch increments failed/raid_failed exactly once.

- [ ] **Step 5: Verify RAID5 behavior**

Run:

```bash
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-raid5-smoke.sh
```

Expected: writesize and capacity match profile; parity rotates 0–3; each single data failure recovers; double failures are rejected.

- [ ] **Step 6: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c \
  rootfs/profile.d/mtd.sh rootfs/init scripts/q3n-raid5-smoke.sh \
  tests/test_scripts.sh
git commit -m "feat: add q3n multiplane page raid5"
```

---

### Task 8: Restore, Erase, Bad-block and Partial-commit Semantics

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `scripts/q3n-raid1-smoke.sh`
- Modify: `scripts/q3n-raid5-smoke.sh`
- Modify: `scripts/q3n-persistence-smoke.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: committed manifest and MP status paths.
- Produces: persistent committed bitmap/generation restore, group erase and group BBM propagation.

- [ ] **Step 1: Add failing persistence/fault stages**

For each profile add: main program fault in one member must return `-EIO` and leave no manifest; after QEMU restart the stripe must not be published. Add OOB program fault after successful main: at least one manifest succeeds, write returns success, restart reads data, and `metadata_degraded` increases. Add a write-only debugfs control `inject_next_manifest_program_fail_plane`; it stores plane 0..3 in `int manifest_fail_plane` under `mtd_lock` (`-1` means disarmed). The write path arms the existing address-targeted QEMU program fault after main success but immediately before OOB commit, then resets the field to `-1`. Add markbad on one logical eraseblock and assert every physical member BBM is bad while main bytes remain unchanged.

- [ ] **Step 2: Verify RED**

Run both RAID wrappers and persistence wrapper; expected failure is missing restore/group semantics, not shell syntax.

- [ ] **Step 3: Implement committed state restore**

Allocate one byte per logical stripe/page row for `EMPTY`, `COMMITTED`, or `CORRUPT`, plus one nonzero generation per logical eraseblock. On persistent-media probe, MP-read member OOBs, accept at least one valid manifest, require all other valid manifests to match byte-for-byte, validate physical placement, and recover the maximum consistent generation. No valid manifest means uncommitted. Conflicts mean corrupt and later reads return `-EBADMSG`.

- [ ] **Step 4: Implement group erase and BBM**

RAID1 erases/checks/marks both pair members; RAID5 uses all four members. Any erase failure sets `fail_addr` to the logical eraseblock and returns `-EIO`. Successful erase clears stripe states, increments nonzero generation, and invalidates cached manifests. `_block_isbad` returns bad when any member BBM is bad. `_block_markbad` writes byte 0 = `0x00` to all members using MP OOB program without changing bytes 1..127 or main/LDPC.

- [ ] **Step 5: Verify persistence and fault matrix**

Run:

```bash
./scripts/q3n-raid1-smoke.sh
./scripts/q3n-raid5-smoke.sh
./scripts/q3n-kunit-smoke.sh
./scripts/q3n-persistence-smoke.sh
```

Expected: all markers present; incomplete main writes remain invisible after restart; partial manifest replication remains readable; group BBM persists and main hashes are unchanged.

- [ ] **Step 6: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  rootfs/profile.d/mtd.sh scripts/q3n-raid1-smoke.sh \
  scripts/q3n-raid5-smoke.sh scripts/q3n-persistence-smoke.sh \
  tests/test_scripts.sh
git commit -m "feat: persist q3n raid commit and bad group state"
```

---

### Task 9: Observability, Documentation and Full Verification

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `README.md`
- Modify: `qemu/README.md`
- Modify: `docs/qemu-3dnand-register-reference.md`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: complete RAID1/RAID5 implementation.
- Produces: stable debugfs surface, user instructions, and fresh full-suite evidence.

- [ ] **Step 1: Add failing documentation/observability gates**

Require these debugfs files and documentation terms:

```text
raid_level
multiplane_commands
multiplane_slot_failures
metadata_degraded
raid_recovered
raid_failed
raid_source_corrected_bits
last_parity_plane
```

Require README examples for `modprobe qemu_3dnand raid_level=1`, default RAID5, 16 KiB/48 KiB write alignment, same-die plane topology, OOB manifest reservation, fresh-image incompatibility, and both host smoke commands.

- [ ] **Step 2: Verify RED, then expose exact counters**

Run `./scripts/smoke-test.sh`, observe missing gates, then add read-only debugfs attributes. `multiplane_commands` and slot failures read QEMU MMIO counters; metadata/recovery counters are Linux `u64` under `mtd_lock`; `last_parity_plane` is `0xff` before the first RAID5 write.

- [ ] **Step 3: Update documentation**

Document the ABI registers/commands, descriptor validation, per-slot ERROR behavior, physical lane formula, RAID1/RAID5 capacity formulas, two-stage main/OOB sequence, failure matrix, and the fact that OOB bytes used by the manifest are driver-private while byte 0 remains BBM.

- [ ] **Step 4: Run static and build verification**

Run fresh:

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/shell.sh ./scripts/configure-kernel.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
```

Expected: every command exits 0; no compiler warning/error; KUnit module builds.

- [ ] **Step 5: Run complete guest verification**

Run fresh:

```bash
./scripts/q3n-raid1-smoke.sh
./scripts/q3n-raid5-smoke.sh
./scripts/q3n-persistence-smoke.sh
./scripts/shell.sh ./scripts/run-qemu.sh --append "MTD_SMOKE=1"
```

Expected: RAID1, RAID5, persistence, and generic MTD markers all appear; no `KUnit: ... failed`, kernel oops, lockdep warning, uncorrectable data leak, or missing debugfs file.

- [ ] **Step 6: Check diff and requirements**

Run:

```bash
git diff --check
git status --short
```

Re-read the design completion criteria and confirm each has a corresponding passing KUnit, host helper, build, or guest smoke result. Confirm `.vscode/` remains the only unrelated untracked path.

- [ ] **Step 7: Commit**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
  rootfs/profile.d/mtd.sh README.md qemu/README.md \
  docs/qemu-3dnand-register-reference.md tests/test_scripts.sh
git commit -m "docs: verify multiplane page raid profiles"
```

---

## Plan Self-review Result

- Spec coverage: same-die topology, RAID1 pairs, RAID5 rotating parity, 16/48 KiB geometry, generic QEMU ABI, OOB commit, recovery, ECC accounting, bad groups, persistence, statistics, documentation and all verification layers map to explicit tasks.
- Type consistency: Task 3 introduces `q3n_raid_group`; Task 4 consumes it for manifest validation; Task 5 produces `q3n_mp_*`; Tasks 6–8 consume both without alternate names.
- Dependency order: QEMU ABI/execution precedes Linux mapping/manifest/transport; RAID1 establishes shared MTD accounting before RAID5; restore and fault semantics follow both profiles.
- Placeholder scan: every task names exact files, interfaces, commands, expected failures, passing evidence and commit boundary; no deferred implementation item remains.
