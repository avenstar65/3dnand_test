# 3D NAND Page-RAID Driver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不修改 Linux MTD core 的前提下，把现有 `qemu_3dnand` direct MTD 原型重构为支持 UBI 16KiB 同步前台访问、串行异步 parity、前台优先调度、物理 block 页序约束、单页 RAID 恢复，并为 ONFI 5.1 双 LUN 四 plane 并行 profile 留出稳定接口。

**Architecture:** MTD `_read/_write/_erase` 继续同步注册；驱动内部以一个前台队列和两个后台 parity 队列统一调度 NAND 命令。串行 profile 中 data page 成功即持久化，parity manifest 表示 RAID 保护完成；QEMU 介质模型强制每个 block 的 page index 单调递增并提供 main+OOB、故障和调度统计。ONFI/并行能力通过独立 backend 接口接入，不传播到 MTD frontend。

**Tech Stack:** Linux 7.0.12、MTD direct callbacks、PCI/MMIO、kernel kthread/completion/spinlock/mutex/mempool、QEMU 11.0.2、ONFI 5.1 capability model、POSIX shell smoke tests、QEMU guest fault injection。

## 实施状态（2026-07-14）

已完成并已提交：Task 1-5；Task 6 的纯 P0/P1/P2 选择器、P1 重排和
KUnit；Task 7 的 16KiB direct MTD 串行布局；Task 10 的基础 guest
端到端 smoke。Task 8 的同 LUN 单页 P1 rebuild 和独立 P2 parity program
已完成实现与验收，等待本阶段提交。当前串行 profile 已固定为同一物理 block 内的
`D0..D6,P`：7 个连续 data page 后写第 8 个 parity page，parity page
不暴露给 MTD。

- 已验证：QEMU 介质页序、qemu-3dnand probe、完整 Linux/rootfs 构建、
  KUnit（P0/P1/P2 和 mapper）、以及 `q3n-serial-smoke`。
- 已验证的 guest 结果：单次连续写 8 个 16KiB data page，不等待第一
  stripe parity；驱动保证 physical page 7 parity 先于下一 stripe 的
  physical page 8 data program。随后注入第一个 data page 丢失，数据由
  同 block parity 恢复，`raid_recovered` 增加。
- 当前已实现：前台请求在等待 `mtd_lock` 前进入 P0；后台 parity 每次只
  claim 一个 P1 请求并读取一个物理 page，完成后重新进入 P1 队尾；第
  7 个 data page rebuild read 完成后，parity program 作为独立 P2 请求
  再次参与调度。data/parity program 共用 per-block `next_prog_page`。
  parity hard limit 按未完成 stripe 预留一次配额，在 D6 编程前施加
  背压，终态释放；`_sync()` 排空 workqueue。
- Task 8 调度子项验收：KUnit 18/18；全新 guest 中先加载 KUnit 后执行
  `q3n-serial-smoke` 成功，`parity_written=1`、`raid_recovered=1`、
  `raid_failed=0`。串行 profile 只验证同 LUN 行为；异 LUN 并行留到
  Task 11/12。rebuild CRC/持久化 manifest 的主路径接线仍未完成，不在
  本次调度子项内宣称完成。
- 页序异常不再永久等待：PROGRAM 落后 `next_prog_page` 返回 `-ESTALE`；
  超前且不存在同 block frontier dependency 返回 `-ERANGE` 并安全摘队。
  guest 已验证 erase 后直接写 D1 会立即失败，不会挂住 MTD 调用线程。
- Task 9 的 generation 和 group cancel barrier 已完成：parity work 在每个
  P1/P2 claim 前复核 generation；erase 按 block 禁止新请求、唤醒并取消
  暂停的旧 parity、等待 pending 归零后再擦除。确定性 guest 验收观测到
  `parity_paused > 0`，且 erase 后 `parity_written`、`raid_failed` 均不变，
  `pending_parity=0`、`reserved_parity=0`；新 generation 的串行写和 RAID
  恢复继续通过。MTD `_block_isbad` 已注册；由于控制器 ABI 仍无持久化
  markbad 能力，`_block_markbad` 返回 `-EOPNOTSUPP`，持久化坏块仍未完成。
  Cancel barrier 由第一个 same-block erase 独占；该 owner 解锁等待期间若有
  后来的同 block erase，则后者设置 `fail_addr` 并返回 `-EBUSY`，不会清除
  第一个 owner 的 cancelling 状态。
- 未开始：Task 11 ONFI backend、Task 12 并行 profile 计划。

## Global Constraints

- 代码仓库：`/Users/yangyu/Documents/linux环境搭建`。
- 新分支：`codex/page-raid-serial-async-priority`，基线 `eb354c1`。
- 当前代码仓库有未提交修改；执行计划前必须在独立 worktree 中检出新分支，不得覆盖现有工作区。
- 不修改 `include/linux/mtd/mtd.h`、`drivers/mtd/mtdcore.c`、`drivers/mtd/ubi/*` 或 `fs/ubifs/*`。
- 第一可交付 profile 固定 `mtd.writesize = physical_page_size = 16KiB`。
- Data program success 表示数据持久化；parity manifest success 表示 stripe 进入 `PROTECTED`。
- 缺少 parity 的已写 data 必须保持可读并标记 `UNPROTECTED`。
- 前台 `_read/_write/_erase` 由 `mtd_lock` 同步串行。
- 调度优先级固定为 P0 foreground、P1 parity rebuild read、P2 parity write。
- Parity rebuild 每次只读一个 page，最多一个后台 rebuild read 在飞。
- 物理 block 首版要求 `request.page == next_prog_page`，不得降序或跳页 program。
- 首个产品 profile 使用同 block 串行 `D0..D6,P` 布局；page 7 是
  parity，随后从 page 8 开始下一 stripe。独立 parity block 映射保留为
  后续 profile 的可选方案，不作为当前实现前提。
- 不实现 FTL、GC、磨损均衡或自动数据迁移。
- 所有开发任务遵循测试先行、小提交和完整错误回滚。

---

## File Structure

代码仓库中最终建议结构：

```text
linux/drivers/mtd/nand/raw/
├── qemu_3dnand_main.c         # PCI probe/remove、MTD 注册、debugfs
├── qemu_3dnand.h              # MMIO ABI，与 QEMU 头保持一致
├── qemu_3dnand_priv.h         # 驱动私有对象、状态和跨文件接口
├── qemu_3dnand_map.c          # 逻辑地址、同 block serial page 映射
├── qemu_3dnand_raid.c         # XOR、CRC、manifest、恢复和 open stripe
├── qemu_3dnand_sched.c        # P0/P1/P2 队列、kthread、completion、背压
└── qemu_3dnand_kunit.c        # mapper、状态机、调度选择和页序测试

qemu/
├── include/hw/mtd/q3n-nand.h  # QEMU ABI、几何、状态和统计寄存器
└── hw/mtd/q3n-nand.c          # main+OOB 介质、页序、命令和故障模型

rootfs/profile.d/mtd.sh        # guest 功能、统计和故障注入命令
tests/test_scripts.sh           # overlay/ABI/配置静态门禁
scripts/smoke-test.sh           # 本地结构和 shell 语法门禁
```

主实现已重命名为 `qemu_3dnand_main.c` 以支持复合模块；最终模块名仍为
`qemu_3dnand.ko`。调度、映射和 RAID 算法拆成独立编译单元。

---

### Task 1: 建立隔离工作区并固定基线（已完成）

**Files:**
- Verify: `/Users/yangyu/Documents/linux环境搭建/.gitignore`
- Verify: `/Users/yangyu/Documents/linux环境搭建/tests/test_scripts.sh`

**Interfaces:**
- Consumes: branch `codex/page-raid-serial-async-priority` at `eb354c1`.
- Produces: clean isolated worktree and recorded baseline test output.

- [ ] **Step 1: 创建独立 worktree**

```bash
cd /Users/yangyu/Documents/linux环境搭建
git check-ignore -q .worktrees
git worktree add .worktrees/page-raid-serial-async codex/page-raid-serial-async-priority
cd .worktrees/page-raid-serial-async
```

Expected: worktree is on `codex/page-raid-serial-async-priority`; original dirty checkout is unchanged.

- [ ] **Step 2: 验证分支基线**

```bash
git status --short
git rev-parse HEAD
```

Expected: empty status and HEAD `eb354c1...`.

- [ ] **Step 3: 运行轻量基线测试**

```bash
./scripts/smoke-test.sh
```

Expected: `ok: script structure verified` and `ok: smoke test passed`.

- [ ] **Step 4: 如果基线失败则停止**

Do not change product code. Record the exact failure and decide whether it is a baseline repair or environment problem before Task 2.

---

### Task 2: 扩展共享 MMIO ABI 和结构门禁（已完成）

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand.h`
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: existing ID/geometry/page command registers.
- Produces: main+OOB command ABI, request class/status statistics, strict ABI equality between Linux and QEMU.

- [ ] **Step 1: 先添加失败的结构断言**

在 `tests/test_scripts.sh` 增加：

```sh
for symbol in \
  Q3N_CMD_READ_PAGE_OOB \
  Q3N_CMD_PROGRAM_PAGE_OOB \
  Q3N_REG_OOB_LEN \
  Q3N_REG_STAT_FG_OPS \
  Q3N_REG_STAT_PARITY_READS \
  Q3N_REG_STAT_PARITY_WRITES \
  Q3N_REG_STAT_ORDER_ERRORS; do
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h "$symbol"
  assert_contains qemu/include/hw/mtd/q3n-nand.h "$symbol"
done
```

- [ ] **Step 2: 运行并确认失败**

Run: `./scripts/smoke-test.sh`

Expected: FAIL on `Q3N_CMD_READ_PAGE_OOB`.

- [ ] **Step 3: 在两侧头文件加入完全一致的 ABI**

```c
#define Q3N_REG_OOB_LEN                 0x0030
#define Q3N_REG_STAT_FG_OPS             0x0070
#define Q3N_REG_STAT_PARITY_READS       0x0074
#define Q3N_REG_STAT_PARITY_WRITES      0x0078
#define Q3N_REG_STAT_ORDER_ERRORS       0x007c

#define Q3N_CMD_READ_PAGE_OOB           6
#define Q3N_CMD_PROGRAM_PAGE_OOB        7
```

保持既有寄存器值不变；新寄存器不得与 IRQ/data window 重叠。

- [ ] **Step 4: 运行结构测试**

Run: `./scripts/smoke-test.sh`

Expected: PASS.

- [ ] **Step 5: 提交**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand.h \
        qemu/include/hw/mtd/q3n-nand.h tests/test_scripts.sh
git commit -m "feat: extend q3n main and OOB command ABI"
```

---

### Task 3: QEMU 介质支持 main+OOB 和严格页序（已完成）

**Files:**
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `qemu/README.md`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 2 ABI.
- Produces: persistent OOB bytes, per-block `next_prog_page`, order error status/statistic.

- [ ] **Step 1: 添加失败的结构测试**

```sh
assert_contains qemu/hw/mtd/q3n-nand.c 'next_prog_page'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_check_program_order'
assert_contains qemu/hw/mtd/q3n-nand.c 'oob_storage'
assert_contains qemu/hw/mtd/q3n-nand.c 'stat_order_errors'
```

- [ ] **Step 2: 运行确认失败**

Run: `./scripts/smoke-test.sh`

Expected: FAIL on `next_prog_page`.

- [ ] **Step 3: 扩展 QEMU state**

```c
uint8_t *main_storage;
uint8_t *oob_storage;
uint32_t *next_prog_page;
uint64_t stat_fg_ops;
uint64_t stat_parity_reads;
uint64_t stat_parity_writes;
uint64_t stat_order_errors;
```

索引 helper 必须使用 64-bit multiplication 并在 realize 时检查总分配溢出。

- [ ] **Step 4: 实现严格页序检查**

```c
static bool q3n_check_program_order(Q3NNandState *s,
                                    uint32_t block, uint32_t page)
{
    if (page != s->next_prog_page[block]) {
        s->stat_order_errors++;
        return false;
    }
    return true;
}
```

只有 main+OOB program 完整成功后执行 `next_prog_page[block]++`；erase 成功后恢复为 0；program fail 不推进。

- [ ] **Step 5: 实现 main+OOB 命令**

`PROGRAM_PAGE_OOB` 从数据窗口依次接收 `page_size` main 和 `oob_size` OOB；`READ_PAGE_OOB` 以相同顺序返回。全 `0xff` 初始化，保持 NAND 1→0 约束。

- [ ] **Step 6: 运行测试和构建 QEMU**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
```

Expected: smoke PASS; QEMU build exits 0.

- [ ] **Step 7: 提交**

```bash
git add qemu/hw/mtd/q3n-nand.c qemu/include/hw/mtd/q3n-nand.h \
        qemu/README.md tests/test_scripts.sh
git commit -m "feat: model OOB data and sequential page programming"
```

---

### Task 4: 拆分驱动私有接口和纯映射层（已完成）

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_map.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Rename: `linux/drivers/mtd/nand/raw/qemu_3dnand.c` to `qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `linux/drivers/mtd/nand/raw/Kconfig.qemu_3dnand`
- Modify: `configs/linux/qemu-x86_64-debug.fragment`

**Interfaces:**
- Consumes: physical page/block geometry.
- Produces: `q3n_map_data_page()`, `q3n_map_parity_page()`, `q3n_program_order_ready()`.

- [ ] **Step 1: 写失败的 KUnit 映射测试**

```c
static void q3n_map_separate_parity_block_test(struct kunit *test)
{
    struct q3n_geometry g = {
        .page_size = 16 * 1024,
        .pages_per_block = 1600,
        .data_pages_per_stripe = 7,
    };
    struct q3n_phys_addr data, parity;

    KUNIT_ASSERT_EQ(test, q3n_map_data_page(&g, 0, 6, &data), 0);
    KUNIT_ASSERT_EQ(test, q3n_map_parity_page(&g, 0, &parity), 0);
    KUNIT_EXPECT_NE(test, data.block, parity.block);
    KUNIT_EXPECT_EQ(test, data.page, 6U);
    KUNIT_EXPECT_EQ(test, parity.page, 0U);
}
```

- [ ] **Step 2: 构建并确认测试缺少符号**

先在 debug fragment 增加：

```text
CONFIG_KUNIT=y
CONFIG_KUNIT_ALL_TESTS=n
CONFIG_MTD_NAND_QEMU_3DNAND_KUNIT_TEST=m
```

Run:

```bash
./scripts/shell.sh ./scripts/configure-kernel.sh
./scripts/shell.sh ./scripts/build-kernel.sh
```

Expected: fail because `q3n_map_data_page` is undefined when KUnit config is enabled.

Kbuild 使用复合模块：`qemu_3dnand-y := qemu_3dnand_main.o
qemu_3dnand_map.o`，保持最终模块名为 `qemu_3dnand.ko`；测试模块由
`qemu_3dnand_kunit.o` 和同一 `qemu_3dnand_map.o` 组成。

- [ ] **Step 3: 定义私有类型和映射接口**

```c
struct q3n_phys_addr { u32 block; u32 page; };

int q3n_map_data_page(const struct q3n_geometry *g, u64 logical_page,
                      u8 slot, struct q3n_phys_addr *out);
int q3n_map_parity_page(const struct q3n_geometry *g, u64 stripe,
                        struct q3n_phys_addr *out);
bool q3n_program_order_ready(const struct q3n_block_state *state,
                             u32 page);
```

使用独立 parity block pool 静态映射；所有除法使用 `div_u64_rem()` 或 `%`，不使用 shift/mask 假设。

- [ ] **Step 4: 实现最小映射并通过 KUnit 编译**

`q3n_program_order_ready()` 首版只接受 `page == next_prog_page`。

- [ ] **Step 5: 运行结构和内核构建测试**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/configure-kernel.sh
./scripts/shell.sh ./scripts/build-kernel.sh
```

Expected: all exit 0.

- [ ] **Step 6: 提交**

```bash
git add linux/drivers/mtd/nand/raw
git commit -m "refactor: add q3n mapping and private driver interfaces"
```

---

### Task 5: 实现 manifest、增量 XOR 和恢复状态（已完成）

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`

**Interfaces:**
- Consumes: Task 4 mapping.
- Produces: `q3n_open_stripe_update()`, `q3n_build_manifest()`, `q3n_validate_manifest()`, `q3n_recover_page()`.

- [ ] **Step 1: 写 XOR/manifest 失败测试**

```c
static void q3n_incremental_xor_test(struct kunit *test)
{
    u8 d0[16] = { 0x55 }, d1[16] = { 0xaa }, parity[16] = {};
    q3n_xor_page(parity, d0, sizeof(parity));
    q3n_xor_page(parity, d1, sizeof(parity));
    KUNIT_EXPECT_EQ(test, parity[0], (u8)0xff);
}
```

再增加 manifest header CRC 损坏返回 `-EBADMSG`、缺 parity 状态为 `Q3N_STRIPE_UNPROTECTED` 的测试。

- [ ] **Step 2: 运行确认失败**

Run:

```bash
./scripts/shell.sh ./scripts/configure-kernel.sh
./scripts/shell.sh ./scripts/build-kernel.sh
```

Expected: undefined RAID helpers or failing assertions.

- [ ] **Step 3: 定义持久化格式**

```c
struct q3n_data_meta {
    __le16 magic;
    u8 version;
    u8 slot;
    __le64 stripe_id;
    __le32 data_crc;
    __le32 header_crc;
} __packed;

struct q3n_parity_manifest {
    __le16 magic;
    u8 version;
    u8 data_pages;
    __le64 stripe_id;
    __le16 member_bitmap;
    __le32 data_crc[7];
    __le32 parity_crc;
    __le32 header_crc;
} __packed;
```

- [ ] **Step 4: 实现增量 XOR 和状态机**

状态固定为 `EMPTY → OPEN/UNPROTECTED → PARITY_QUEUED → PROTECTED`，失败转为 `PARITY_FAILED/UNPROTECTED`。Data program failure不得回滚此前成功 data。

- [ ] **Step 5: 运行 KUnit 和构建**

Expected: XOR、CRC、manifest、状态转换测试全部 PASS.

- [ ] **Step 6: 提交**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
        linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c
git commit -m "feat: add serial page RAID metadata and XOR state machine"
```

---

### Task 6: 实现 P0/P1/P2 调度器和锁拆分（部分完成）

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`

**Interfaces:**
- Consumes: physical request and page-order predicate.
- Produces: `q3n_sched_enqueue()`, `q3n_sched_pick_next()`, `q3n_sched_drain()`, completion lifecycle.

- [ ] **Step 1: 写失败的调度测试**

覆盖：P0 优先、P1 优先于 P2、不可执行的高优先级请求不阻塞 ready parity barrier、page 小于前沿被拒绝、page 大于前沿且缺失请求不存在被拒绝。

```c
KUNIT_EXPECT_PTR_EQ(test, q3n_sched_pick_next(&sched), fg_req);
```

- [ ] **Step 2: 运行确认失败**

Expected: scheduler helpers undefined.

- [ ] **Step 3: 定义锁和队列**

```c
struct mutex mtd_lock;
spinlock_t sched_lock;
struct list_head foreground_queue;
struct list_head parity_read_queue;
struct list_head parity_write_queue;
wait_queue_head_t sched_waitq;
atomic_t inflight;
```

调度/IRQ 路径禁止获取 `mtd_lock`；不得在持有 `sched_lock` 时等待硬件。

- [ ] **Step 4: 实现选择器**

顺序固定 P0→P1→P2，只返回 `q3n_req_ready()` 的请求。普通 read 不检查 `next_prog_page`；program 必须精确匹配。

- [ ] **Step 5: 增加 parity 水位**

```c
#define Q3N_MAX_PENDING_PARITY 32
#define Q3N_MAX_PARITY_READ_INFLIGHT 1
```

达到 parity hard limit 后只阻塞新 data write，不阻塞前台 read。

- [ ] **Step 6: 运行 KUnit/lockdep 构建并提交**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
        linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c
git commit -m "feat: add foreground-priority NAND request scheduler"
```

---

### Task 7: 接入 Direct MTD 16KiB 前台路径（部分完成）

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Tasks 4-6.
- Produces: synchronous `_read/_write/_erase/_sync`, data main+OOB persistence, async parity enqueue.

- [ ] **Step 1: 添加结构门禁**

```sh
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.c 'mtd->_sync'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.c 'mtd_lock'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.c 'q3n_sched_enqueue'
assert_not_contains linux/drivers/mtd/nand/raw/qemu_3dnand.c 'IS_ALIGNED.*mtd->erasesize'
```

- [ ] **Step 2: 运行确认失败**

Run: `./scripts/smoke-test.sh`.

- [ ] **Step 3: 重写 `_write()`**

要求 16KiB page 对齐；持有 `mtd_lock`；构造 P0 data request；等待 data main+OOB program 完成后更新 accumulator；stripe 满时把 buffer 所有权转移到 P2 parity request；随后即可返回该 data page成功。

- [ ] **Step 4: 重写 `_read()`**

正常 read 进入 P0；UNPROTECTED stripe 直接读取 data；只有有效 parity manifest 且目标 data ECC 不可纠时才进入 RAID recovery。

- [ ] **Step 5: 重写 `_erase()` 和 `_sync()`**

`_erase()` 使用 `% mtd->erasesize`，等待/取消目标 group 后台请求并推进 generation；`_sync()` 排空 P1/P2。不得用 `IS_ALIGNED()` 检查非 2 次幂 erasesize。

- [ ] **Step 6: 配置 MTD 几何**

```c
mtd->writesize = q3n->page_size;
mtd->writebufsize = q3n->page_size;
mtd->erasesize = q3n_serial_data_bytes_per_block(q3n);
mtd->_sync = q3n_mtd_sync;
```

- [ ] **Step 7: 运行构建和结构测试**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-kernel.sh
```

- [ ] **Step 8: 提交**

```bash
git add linux/drivers/mtd/nand/raw tests/test_scripts.sh
git commit -m "feat: expose 16KiB MTD writes with async parity protection"
```

---

### Task 8: 实现分片 parity rebuild read（调度子项已完成）

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`

**Interfaces:**
- Consumes: P1 queue and data metadata.
- Produces: one-page-at-a-time accumulator reconstruction without blocking a full `N × tR` interval.

- [ ] **Step 1: 写失败测试**

创建 7-page rebuild，断言一次 completion 后 `next_slot` 只增加 1，且请求重新入 P1 队尾；插入 P0 后下一次选择 P0。

- [ ] **Step 2: 实现 rebuild context**

```c
struct q3n_parity_rebuild {
    u64 stripe_id;
    u16 member_bitmap;
    u8 next_slot;
    u8 *parity_accumulator;
    u32 data_crc[7];
};
```

- [ ] **Step 3: 实现单步函数**

```c
int q3n_rebuild_read_one(struct q3n_device *q3n,
                         struct q3n_parity_rebuild *rb);
```

一次只发一个 physical page read；完成后 XOR、更新 CRC并重新调度。Read 不改变任何 `next_prog_page`。

- [ ] **Step 4: 验证同/异 LUN 行为**

同 LUN 前台最多等待一个已启动 `tR`；不同空闲 LUN 允许 P0 与 P1 并行。持续 P0 压力只对新 data write 施加 parity 水位背压。

- [ ] **Step 5: 运行测试并提交**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c
git commit -m "feat: rebuild parity with preemptible single-page reads"
```

---

### Task 9: 坏块、generation 和后台请求取消（cancel barrier 已完成）

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_map.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`

**Interfaces:**
- Produces: `_block_isbad`, optional `_block_markbad`, stale worker prevention.

- [x] **Step 1: 写 generation 失败测试**

构造 generation=1 的 parity request，模拟 erase 将 group generation 增到 2，断言 worker 返回 `-ESTALE` 且不 program。

- [x] **Step 2: 实现 group barrier**

Erase 前阻止目标 group 新请求，取消未提交 parity，等待 active request；成功 erase 后增加非零 generation 并清除 validation bitmap。

- [x] **Step 3: 注册坏块钩子**

任一 data/parity block 坏则逻辑 group bad。不做替换映射；没有持久化 markbad 能力时 `_block_markbad` 返回 `-EOPNOTSUPP`。

- [x] **Step 4: 运行 KUnit、构建并提交**

```bash
git add linux/drivers/mtd/nand/raw
git commit -m "feat: coordinate bad groups and stale parity requests"
```

确定性验收结果：worker 在 claim 前达到 `parity_paused > 0`；目标 block
erase 返回后 `parity_written` 与 `raid_failed` 保持不变，
`pending_parity=0`、`reserved_parity=0`。随后新 generation 写入和单页
Page-RAID 恢复通过。

- [ ] **后续：实现控制器持久化 markbad ABI 和掉电后坏块状态恢复**

---

### Task 10: Guest 工具和端到端串行验证（部分完成）

**Files:**
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `rootfs/init`
- Modify: `tests/test_scripts.sh`
- Modify: `README.md`

**Interfaces:**
- Produces: reproducible guest commands for priority, order, parity state and fault tests.

- [ ] **Step 1: 添加失败的命令门禁**

```sh
assert_contains rootfs/profile.d/mtd.sh 'q3n-serial-smoke'
assert_contains rootfs/profile.d/mtd.sh 'q3n-parity-stats'
assert_contains rootfs/profile.d/mtd.sh 'order_errors'
assert_contains rootfs/profile.d/mtd.sh 'unprotected'
```

- [ ] **Step 2: 实现 guest 测试命令**

`q3n-serial-smoke` 依次执行：erase、逐 16KiB 写 7 页、每页立即读回、检查 parity queued/protected、注入 parity fail、确认 data 仍可读。

- [ ] **Step 3: 增加页序和优先级统计**

输出 foreground ops、parity reads/writes、order errors、protected/unprotected/failed stripe 和最大 pending parity。

- [ ] **Step 4: 运行完整构建和 QEMU smoke**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/shell.sh ./scripts/run-qemu.sh --append "MTD_SMOKE=1"
```

Expected: all exit 0; `order_errors=0`; data readable before/after parity failure; protected count increases after full stripe.

- [ ] **Step 5: 提交**

```bash
git add rootfs tests README.md
git commit -m "test: cover serial asynchronous page RAID in QEMU"
```

---

### Task 11: 增加 ONFI Backend 边界，不启用并行 profile

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_onfi.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`

**Interfaces:**
- Produces: `struct q3n_onfi_ops`, capability-driven 4-plane/2-plane/single-plane selection.

- [ ] **Step 1: 写 capability 降级测试**

输入 2 LUN/4 plane but no multi-plane capability，断言选择 single-plane + multi-LUN；输入 max 2-plane，断言拆成 two 2-plane groups。

- [ ] **Step 2: 定义 backend 接口**

```c
struct q3n_onfi_ops {
    int (*discover)(struct q3n_device *, struct q3n_geometry *);
    int (*submit_lun)(struct q3n_device *, struct q3n_lun_req *);
    int (*read_lun_status)(struct q3n_device *, u8,
                           struct q3n_lun_status *);
    int (*reset_lun)(struct q3n_device *, u8);
};
```

- [ ] **Step 3: 实现 capability planner**

只生成计划/descriptor group，不把 80h/81h/11h/10h 硬编码到 RAID 层。第一阶段 QEMU backend 仍可串行执行。

- [ ] **Step 4: 测试并提交**

```bash
git add linux/drivers/mtd/nand/raw
git commit -m "refactor: add capability-driven ONFI backend boundary"
```

---

### Task 12: 另立并行 7D+1P 实施计划

**Files:**
- Create: `docs/superpowers/plans/2026-07-11-3dnand-onfi-parallel-profile.md` in the code repository.

**Interfaces:**
- Consumes: stable serial scheduler, mapper, manifest and ONFI backend from Tasks 1-11.
- Produces: independently reviewable plan for 112KiB direct MTD profile and QEMU 2-LUN × 4-plane command engine.

- [ ] **Step 1: 用当前实现证据重新核对范围**

记录 controller 是否具备 per-LUN context、DMA/descriptor、enhanced status 和 4-plane capability；不能用设计假设替代实现证据。

- [ ] **Step 2: 将并行工作拆为独立任务**

至少覆盖：QEMU per-LUN busy/status、4-plane command sequence、112KiB MTD profile、8-page status aggregation、manifest candidate validation、non-power-of-two UBI 明确保持 unsupported。

- [ ] **Step 3: 提交计划而不夹带产品代码**

```bash
git add docs/superpowers/plans
git commit -m "docs: plan ONFI parallel page RAID profile"
```

---

## Final Verification Gate

执行全部任务后，必须重新运行：

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/shell.sh ./scripts/configure-kernel.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/shell.sh ./scripts/run-qemu.sh --append "MTD_SMOKE=1"
./scripts/shell.sh ./scripts/run-qemu.sh --append "MTD_SMOKE=ubifs"
git status --short
```

验收标准：

- 所有命令退出 0。
- `mtd.writesize = 16384`，原生 UBI attach 不触发非 2 次幂检查。
- 单页 data 写完成后立即可读，缺 parity 时状态为 UNPROTECTED 而非丢失。
- 完整 stripe 最终进入 PROTECTED。
- parity rebuild 不连续占用 N 个 page read；前台请求优先。
- 每个物理 block 的 program page index 严格单调，`order_errors=0`。
- parity program fail 不回滚已经成功的 data。
- erase/suspend/remove 不允许 stale parity worker 写回新 generation。
- 代码工作区 clean；原始 dirty checkout 完全未被修改。
