# QEMU 3D NAND Persistent Physical Media Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 QEMU BlockBackend 稀疏镜像持久化完整物理 NAND，并让 Linux 驱动从物理编程前沿恢复串行 `D0..D6,P` 状态及使用首 page `OOB[0]` 的持久化 markbad。

**Architecture:** QEMU 只保存固定物理 page slot、page state、物理编程游标和 OOB BBM，不解析 RAID 布局。Linux 在 probe 时查询物理 block status，自行重建 data-valid、parity index 和尾部 parity；两次正常 QEMU 启动验证介质、恢复和坏块状态。

**Tech Stack:** QEMU 11.0.2 BlockBackend/QOM/PCI、Linux 7.0.12 MTD direct callbacks、KUnit、BusyBox initramfs、mtd-utils、POSIX shell。

## Global Constraints

- 工作区固定为 `/Users/yangyu/Documents/3dnand-page-raid-serial-worktree`，分支固定为 `codex/page-raid-serial-async-priority`。
- QEMU 只管理物理 NAND；不得加入 `D0..D6,P`、stripe、parity、generation、FTL 或 GC 语义。
- 串行布局保持同一物理 block 内 `D0..D6,P`，严格升序 program。
- BBM 唯一事实源是物理 block 首个 page 的 `OOB[0]`：`0xff` good，`0x00` bad。
- `MARK_BAD_BLOCK` 是专用命令，不增加通用 OOB-only program ABI，不推进 `next_prog_page`。
- 只保证正常 QEMU 关闭后的恢复；不实现 `kill -9`/主机掉电事务日志。
- 不修改 `include/linux/mtd/mtd.h`、`drivers/mtd/mtdcore.c`、`drivers/mtd/ubi/*` 或 `fs/ubifs/*`。
- P0/P1/P2 优先级、erase cancel ownership、parity commit 和 generation 规则保持不变。
- 不实现坏块替换、reserve 分配、wear leveling、multi-plane/multi-die 并行或镜像升级。
- 每个任务严格 RED→GREEN，提交前运行该任务列出的验证命令。

---

## File Structure

```text
qemu/include/hw/mtd/q3n-media.h
    镜像 v1 格式、物理 page state 和 BlockBackend 介质接口
qemu/hw/mtd/q3n-media.c
    新建/校验镜像、固定偏移 main/OOB 读写、erase、lost state、BBM
qemu/include/hw/mtd/q3n-nand.h
    物理 controller ABI 和 media setter
qemu/hw/mtd/q3n-nand.c
    MMIO 命令执行与 stats，不包含 RAID 语义
qemu/hw/mtd/q3n-pci.c
    PCI drive 属性和 BlockBackend 转交
qemu/hw/mtd/meson.build
    q3n-media.c 构建入口
scripts/apply-qemu-overlay.sh
    新 media 文件复制到 QEMU 11.0.2 源码树
scripts/run-qemu.sh
    默认镜像、--fresh-nand、--nand-image
linux/drivers/mtd/nand/raw/qemu_3dnand.h
    与 QEMU 完全一致的 ABI
linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h
linux/drivers/mtd/nand/raw/qemu_3dnand_map.c
linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c
    可单测的物理 frontier -> 串行布局重放
linux/drivers/mtd/nand/raw/qemu_3dnand_main.c
    probe 恢复、尾部 parity、markbad 和 bad cache
rootfs/helpers/mtd_badblock.c
    MEMGETBADBLOCK/MEMSETBADBLOCK guest 验收工具
rootfs/profile.d/mtd.sh
rootfs/init
scripts/q3n-persistence-smoke.sh
    两次 QEMU 正常启动的端到端验收
tests/test_scripts.sh
    ABI、文件、CLI 和 guest 命令结构门禁
```

---

### Task 1: 增加 BlockBackend 稀疏物理介质和启动参数

**Files:**
- Create: `qemu/include/hw/mtd/q3n-media.h`
- Create: `qemu/hw/mtd/q3n-media.c`
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `qemu/hw/mtd/q3n-pci.c`
- Modify: `qemu/hw/mtd/meson.build`
- Modify: `scripts/apply-qemu-overlay.sh`
- Modify: `scripts/run-qemu.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces: `Q3NMedia`、`q3n_media_open()`、`q3n_media_close()`。
- Produces: `q3n_media_read_page()`、`q3n_media_program_page()`、`q3n_media_erase_block()`、`q3n_media_inject_loss()`。
- Produces CLI: `--fresh-nand`、`--nand-image PATH`，默认 `work/media/q3n-nand.raw`。

- [x] **Step 1: 添加失败的结构门禁**

在 `tests/test_scripts.sh` 增加：

```sh
for file in qemu/include/hw/mtd/q3n-media.h qemu/hw/mtd/q3n-media.c; do
  [ -f "$repo_root/$file" ] || fail "missing $file"
done
assert_contains qemu/hw/mtd/q3n-pci.c 'DEFINE_PROP_DRIVE\("drive"'
assert_contains qemu/hw/mtd/q3n-media.c 'Q3NMEDIA'
assert_contains qemu/hw/mtd/q3n-media.c 'blk_truncate'
assert_contains qemu/hw/mtd/q3n-media.c 'blk_pread'
assert_contains qemu/hw/mtd/q3n-media.c 'blk_pwrite'
assert_contains scripts/run-qemu.sh '--fresh-nand'
assert_contains scripts/run-qemu.sh '--nand-image'
assert_contains scripts/run-qemu.sh 'q3n-nand-pci,drive=q3n-media'
```

- [x] **Step 2: 运行 RED**

Run: `./scripts/smoke-test.sh`

Expected: exit 1，首先失败于缺少 `q3n-media.h`。

- [x] **Step 3: 定义镜像格式与 media API**

在 `q3n-media.h` 定义：

```c
#define Q3N_MEDIA_MAGIC "Q3NMEDIA"
#define Q3N_MEDIA_VERSION 1
#define Q3N_MEDIA_HEADER_SIZE 4096

enum q3n_page_state {
    Q3N_PAGE_ERASED = 0,
    Q3N_PAGE_PRESENT = 1,
    Q3N_PAGE_LOST = 2,
};

typedef struct QEMU_PACKED Q3NMediaHeader {
    uint8_t magic[8];
    uint32_t version_le;
    uint32_t header_size_le;
    uint32_t page_size_le;
    uint32_t oob_size_le;
    uint32_t pages_per_block_le;
    uint32_t physical_block_count_le;
    uint64_t block_state_offset_le;
    uint64_t block_state_length_le;
    uint64_t page_state_offset_le;
    uint64_t page_state_length_le;
    uint64_t page_slot_offset_le;
    uint64_t page_slot_stride_le;
    uint64_t image_size_le;
    uint8_t reserved[4008];
} Q3NMediaHeader;

typedef struct Q3NMedia {
    BlockBackend *blk;
    uint32_t page_size;
    uint32_t oob_size;
    uint32_t pages_per_block;
    uint32_t block_count;
    uint64_t page_slot_offset;
    uint64_t page_slot_stride;
    uint32_t *next_prog_page;
    uint8_t *page_state;
    bool *bad;
} Q3NMedia;

int q3n_media_open(Q3NMedia *m, BlockBackend *blk, uint32_t page_size,
                   uint32_t oob_size, uint32_t pages_per_block,
                   uint32_t block_count, Error **errp);
void q3n_media_close(Q3NMedia *m);
int q3n_media_read_page(Q3NMedia *m, uint32_t block, uint32_t page,
                        uint8_t *data, uint8_t *oob);
int q3n_media_program_page(Q3NMedia *m, uint32_t block, uint32_t page,
                           const uint8_t *data, const uint8_t *oob);
int q3n_media_erase_block(Q3NMedia *m, uint32_t block);
int q3n_media_inject_loss(Q3NMedia *m, uint32_t block, uint32_t page);
```

加入编译期断言：

```c
QEMU_BUILD_BUG_ON(sizeof(Q3NMediaHeader) != Q3N_MEDIA_HEADER_SIZE);
```

- [x] **Step 4: 实现新建、校验和固定偏移读写**

`q3n_media_open()` 必须区分 `blk_getlength(blk) == 0` 与已有镜像。新镜像使用：

```c
ret = blk_truncate(blk, image_size, true, PREALLOC_MODE_OFF, 0, errp);
```

依次写 header、零初始化的 block/page state，并把每个 block 首 page slot 的
OOB byte 0 写为 `0xff`。已有镜像必须完整校验 header、长度和所有 page state
值，只允许 0、1、2。

page slot offset 统一使用：

```c
static uint64_t q3n_media_page_offset(const Q3NMedia *m,
                                      uint32_t block, uint32_t page)
{
    uint64_t index = (uint64_t)block * m->pages_per_block + page;
    return m->page_slot_offset + index * m->page_slot_stride;
}
```

读取 erased page 时 main/OOB 先填 `0xff`；page 0 仍从 slot 读取真实 BBM。
program 先写完整 main+OOB slot，再写 page state 和 block frontier，最后更新内存。
erase 只把对应 page-state 范围清零并把 frontier 写 0，不覆盖旧 slot。lost 把
page state 从 PRESENT 写为 LOST。

- [x] **Step 5: 接入 q3n-nand 和 PCI drive**

删除 `Q3NPage` 哈希表以及 controller state 中重复的 `block_meta`、
`page_valid`、`next_prog_page` 数组。`q3n_read_page()`、program、erase、
program-order 检查和 fault injection 全部调用 media API；stats 仍由
controller 层更新，避免物理状态存在两个事实源。

在 PCI state 增加：

```c
BlockBackend *blk;
```

属性和 realize 顺序：

```c
static const Property q3n_pci_properties[] = {
    DEFINE_PROP_DRIVE("drive", Q3NNandPciState, blk),
};

q3n_nand_set_blk(s->nand, s->blk);
if (!sysbus_realize(SYS_BUS_DEVICE(nand_dev), errp)) {
    object_unref(OBJECT(s->nand));
    s->nand = NULL;
    return;
}
```

没有 backend 时内部 NAND realize 返回 `error_setg(errp, "q3n-nand requires a drive")`。

- [x] **Step 6: 更新 overlay 和 run-qemu CLI**

`apply-qemu-overlay.sh` 复制新 `.c/.h` 并让 meson 同时构建：

```meson
system_ss.add(when: 'CONFIG_Q3N_NAND',
              if_true: files('q3n-media.c', 'q3n-nand.c', 'q3n-pci.c'))
```

`run-qemu.sh` 解析完参数后执行：

```sh
nand_image=${nand_image:-"$work_dir/media/q3n-nand.raw"}
case "$nand_image" in
  /*) ;;
  *) nand_image="$repo_root/$nand_image" ;;
esac
mkdir -p "$(dirname "$nand_image")"
[ "$fresh_nand" -eq 0 ] || rm -f "$nand_image"
[ -e "$nand_image" ] || : > "$nand_image"
```

QEMU 参数增加：

```sh
-drive "if=none,id=q3n-media,format=raw,file=$nand_image" \
-device q3n-nand-pci,drive=q3n-media
```

- [x] **Step 7: GREEN 构建和空镜像重开**

Run:

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/run-qemu.sh --fresh-nand --append "MTD_SMOKE=1"
./scripts/run-qemu.sh --append "MTD_SMOKE=1"
```

Expected: 两次 QEMU 都正常 poweroff；`work/media/q3n-nand.raw` 为 sparse 文件，
开头 8 bytes 为 `Q3NMEDIA`。

再创建损坏镜像并确认设备拒绝 realize：

```bash
printf 'BADMEDIA' > work/media/q3n-invalid.raw
if ./scripts/run-qemu.sh --nand-image work/media/q3n-invalid.raw \
     --append "MTD_SMOKE=1"; then
  echo "invalid media unexpectedly accepted" >&2
  exit 1
fi
```

Expected: QEMU exit non-zero，错误包含 invalid q3n media header/length。

- [ ] **Step 8: 提交**

```bash
git add qemu scripts tests/test_scripts.sh
git commit -m "feat: persist physical NAND media"
```

---

### Task 2: 增加物理 block status 和 OOB BBM 控制器 ABI

**Files:**
- Modify: `qemu/include/hw/mtd/q3n-media.h`
- Modify: `qemu/hw/mtd/q3n-media.c`
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand.h`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces: `q3n_media_get_block_status()`、`q3n_media_mark_bad()`。
- Produces ABI: `Q3N_CMD_GET_BLOCK_STATUS=8`、`Q3N_CMD_MARK_BAD_BLOCK=9`。
- Produces registers: `Q3N_REG_BLOCK_STATUS=0x0080`、`Q3N_REG_BLOCK_NEXT_PAGE=0x0084`。

- [x] **Step 1: 添加失败的 ABI 同步门禁**

```sh
for symbol in Q3N_CAP_PERSISTENT_MEDIA Q3N_CAP_BAD_BLOCK_MARKER \
              Q3N_CMD_GET_BLOCK_STATUS Q3N_CMD_MARK_BAD_BLOCK \
              Q3N_REG_BLOCK_STATUS Q3N_REG_BLOCK_NEXT_PAGE; do
  assert_contains qemu/include/hw/mtd/q3n-nand.h "$symbol"
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h "$symbol"
done
assert_contains qemu/include/hw/mtd/q3n-media.h 'Q3N_BBM_GOOD.*0xff'
assert_contains qemu/include/hw/mtd/q3n-media.h 'Q3N_BBM_BAD.*0x00'
```

- [x] **Step 2: 运行 RED**

Run: `./scripts/smoke-test.sh`

Expected: FAIL on `Q3N_CAP_PERSISTENT_MEDIA`。

- [x] **Step 3: 定义完全一致的 ABI**

双方头文件增加：

```c
#define Q3N_CAP_BASIC_FLASH             (1U << 0)
#define Q3N_CAP_PERSISTENT_MEDIA        (1U << 1)
#define Q3N_CAP_BAD_BLOCK_MARKER        (1U << 2)
#define Q3N_BLOCK_STATUS_BAD            (1U << 0)
#define Q3N_BLOCK_STATUS_ERASED         (1U << 1)
```

QEMU enum 和 Linux define 使用相同数值：

```c
Q3N_REG_BLOCK_STATUS = 0x0080,
Q3N_REG_BLOCK_NEXT_PAGE = 0x0084,
Q3N_CMD_GET_BLOCK_STATUS = 8,
Q3N_CMD_MARK_BAD_BLOCK = 9,
```

- [x] **Step 4: 实现物理状态和专用 BBM program**

media 接口：

```c
int q3n_media_get_block_status(Q3NMedia *m, uint32_t block,
                               uint32_t *status, uint32_t *next_page);
int q3n_media_mark_bad(Q3NMedia *m, uint32_t block);
```

`get` 从首 page slot 的 OOB offset 0 读取 BBM，不读取独立 bad 字段。
`mark_bad` 只向该字节写 `0x00`，不修改 page state/frontier；重复调用返回 0。
成功写入后才更新 `m->bad[block]`。program/erase 查询该 cache 并返回 `-EIO`，
read 不因 bad 被拒绝。

`q3n-media.h` 同时定义：

```c
#define Q3N_BBM_GOOD 0xff
#define Q3N_BBM_BAD  0x00
```

controller 命令要求 `addr` 对齐 `erase_size`，把结果锁存在两个新 MMIO
register 中。非法地址或 media error 进入 `q3n_finish_error()`。

- [x] **Step 5: GREEN 验证**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/shell.sh ./scripts/apply-linux-overlay.sh
./scripts/shell.sh make -C /workspace/work/linux/linux-7.0.12 \
  O=/workspace/work/build/linux-7.0.12 ARCH=x86_64 \
  CROSS_COMPILE=x86_64-linux-gnu- M=drivers/mtd/nand/raw modules
```

Expected: all exit 0。

- [ ] **Step 6: 提交**

```bash
git add qemu linux/drivers/mtd/nand/raw/qemu_3dnand.h tests/test_scripts.sh
git commit -m "feat: expose persistent NAND block status"
```

---

### Task 3: Linux probe 重放物理编程前沿和尾部 parity

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_map.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces: `q3n_replay_serial_frontier()` pure helper。
- Produces: `qemu_3dnand_get_phys_block_status_locked()`。
- Produces: `qemu_3dnand_restore_media_locked()`，在 MTD 注册前调用。

- [x] **Step 1: 写失败的 KUnit**

接口：

```c
int q3n_replay_serial_frontier(u32 pages_per_block, u32 next_prog_page,
                               u8 *data_valid, u8 *parity_valid,
                               bool *needs_tail_parity);
```

新增测试：

```c
static void q3n_replay_serial_frontier_test(struct kunit *test)
{
    u8 data_valid[16] = {};
    u8 parity_valid[2] = {};
    bool needs_tail;

    KUNIT_ASSERT_EQ(test, q3n_replay_serial_frontier(16, 10,
                    data_valid, parity_valid, &needs_tail), 0);
    KUNIT_EXPECT_EQ(test, data_valid[0], 1);
    KUNIT_EXPECT_EQ(test, data_valid[6], 1);
    KUNIT_EXPECT_EQ(test, data_valid[7], 0);
    KUNIT_EXPECT_EQ(test, data_valid[8], 1);
    KUNIT_EXPECT_EQ(test, data_valid[9], 1);
    KUNIT_EXPECT_EQ(test, parity_valid[0], 1);
    KUNIT_EXPECT_EQ(test, parity_valid[1], 0);
    KUNIT_EXPECT_FALSE(test, needs_tail);

    memset(data_valid, 0, sizeof(data_valid));
    memset(parity_valid, 0, sizeof(parity_valid));
    KUNIT_ASSERT_EQ(test, q3n_replay_serial_frontier(16, 7,
                    data_valid, parity_valid, &needs_tail), 0);
    KUNIT_EXPECT_TRUE(test, needs_tail);
    KUNIT_EXPECT_EQ(test, q3n_replay_serial_frontier(16, 17,
                    data_valid, parity_valid, &needs_tail), -EINVAL);
}
```

- [x] **Step 2: 构建并确认 RED**

Run: Task 2 的 raw NAND module build。

Expected: compile/link 失败，缺少 `q3n_replay_serial_frontier`。

- [x] **Step 3: 实现 pure replay helper**

helper 先清空 `pages_per_block` 个 data-valid byte 和
`pages_per_block / 8` 个 parity-valid byte，然后遍历 `[0,next_prog_page)`：

```c
if (page % 8 == 7)
    parity_valid[page / 8] = 1;
else
    data_valid[page] = 1;
*needs_tail_parity = next_prog_page % 8 == 7;
```

拒绝 `pages_per_block == 0`、非 8 整除、`next_prog_page > pages_per_block` 或
NULL 参数。

- [x] **Step 4: 实现 status query 和 probe 重放**

status helper 设置 block 首 page 物理地址，执行 GET 命令并读取：

```c
*status = qemu_3dnand_readl(q3n, Q3N_REG_BLOCK_STATUS);
*next_page = qemu_3dnand_readl(q3n, Q3N_REG_BLOCK_NEXT_PAGE);
```

`restore_media_locked()` 对每个 data block：

- 设置 bad、erased、program frontier；
- 调 pure helper 填充 `data_page_valid`；
- 对 `parity_valid[stripe]` 建立 deterministic parity entry，物理位置为
  `block, stripe * 8 + 7`，generation 引用当前 data generation；
- scan 顺序递增 `parity_sequence`。

bad block 只恢复 bad cache 和物理 frontier，用于诊断；不得尝试补尾部 parity，
也不得把该 block 的 parity entry 标记为可恢复数据源。

调用点必须位于 buffers、metadata、workqueue 和 mutex 初始化之后，
`mtd_device_register()` 之前。

- [x] **Step 5: 完成余数 7 的 parity**

若 helper 返回 `needs_tail_parity=true`，在 probe 单线程上下文调用现有同步
`qemu_3dnand_append_parity_locked(q3n, block, next_page / 8)`。成功后 frontier
必须为 `next_page + 1` 且 parity entry valid；失败时 probe 返回错误并清理资源。
该函数从本任务起是 probe 恢复路径的正式入口，移除其 `__maybe_unused` 标记。

- [x] **Step 6: GREEN 验证**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/apply-linux-overlay.sh
./scripts/shell.sh make -C /workspace/work/linux/linux-7.0.12 \
  O=/workspace/work/build/linux-7.0.12 ARCH=x86_64 \
  CROSS_COMPILE=x86_64-linux-gnu- M=drivers/mtd/nand/raw modules
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
```

启动 fresh QEMU，加载 `qemu_3dnand_test`。Expected: 新 KUnit case 和全部旧 case
pass，guest serial smoke pass。

- [ ] **Step 7: 提交**

```bash
git add linux/drivers/mtd/nand/raw tests/test_scripts.sh
git commit -m "feat: replay serial RAID state from NAND"
```

---

### Task 4: 实现 Linux markbad 和 guest ioctl 工具

**Files:**
- Create: `rootfs/helpers/mtd_badblock.c`
- Modify: `scripts/build-rootfs.sh`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces: functional MTD `_block_markbad()`。
- Produces guest tool: `/usr/sbin/mtd_badblock get|set DEVICE OFFSET`。
- Produces guest command: `q3n-markbad-smoke`。

- [x] **Step 1: 添加失败的结构门禁**

```sh
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CMD_MARK_BAD_BLOCK'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_CAP_BAD_BLOCK_MARKER'
assert_contains scripts/build-rootfs.sh 'mtd_badblock.c'
assert_contains rootfs/profile.d/mtd.sh 'q3n-markbad-smoke'
```

- [x] **Step 2: 运行 RED**

Run: `./scripts/smoke-test.sh`

Expected: FAIL on `Q3N_CMD_MARK_BAD_BLOCK` in the driver。

- [x] **Step 3: 实现 markbad owner 路径**

增加物理 helper：

```c
static int qemu_3dnand_mark_phys_block_bad_locked(struct qemu_3dnand *q3n,
                                                   u32 block)
{
    qemu_3dnand_set_addr(q3n, qemu_3dnand_phys_addr(q3n, block, 0));
    qemu_3dnand_writel(q3n, Q3N_REG_CMD, Q3N_CMD_MARK_BAD_BLOCK);
    return qemu_3dnand_wait_ready(q3n);
}
```

`_block_markbad()` 必须：

- 计算 block 并持 `mtd_lock`；
- cache 已 bad 时幂等返回 0；
- 调 `qemu_3dnand_cancel_block_parity()` 获取 owner 并 drain；
- 执行物理 mark；
- 仅成功后设置 `data_meta[block].bad=true`；
- owner 的成功/失败路径恰好一次 `q3n_block_cancel_end()`；
- 未获得 owner 的路径返回 `-EBUSY` 且不 cancel_end。

write/erase 在物理命令前检查 cache bad 并返回 `-EIO`。read 不增加 bad 拒绝。

- [x] **Step 4: 实现最小 ioctl 工具**

`mtd_badblock.c` 使用：

```c
#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

if (!strcmp(argv[1], "get"))
    ret = ioctl(fd, MEMGETBADBLOCK, &offset);
else if (!strcmp(argv[1], "set"))
    ret = ioctl(fd, MEMSETBADBLOCK, &offset);
else
    return 2;
```

要求 `argc == 4`，用 `strtoll(argv[3], &end, 0)` 严格解析 offset；get 打印
`0` 或 `1`，set 成功打印 `marked`，ioctl error 使用 `perror` 并 exit 1。

`build-rootfs.sh` 在生成 cpio 前执行：

```sh
${ROOTFS_CC:-x86_64-linux-gnu-gcc} -O2 -Wall -Wextra \
  -o "$stage/usr/sbin/mtd_badblock" \
  "$repo_root/rootfs/helpers/mtd_badblock.c"
```

- [x] **Step 5: 实现单次启动 markbad smoke**

`mtd_q3n_markbad_smoke()`：

1. 加载 q3n，读取 `/sys/class/mtd/mtdN/erasesize`；
2. 使用 block 1 offset 调 `mtd_badblock set`；
3. `get` 必须输出 1；
4. 对该 block 的一页 dd write 必须失败；
5. `flash_erase -q -N "$mtd_dev" "$erasesize" 1` 输出必须包含
   `MTD Erase failure`（当前 mtd-utils 即使 ioctl 返回 EIO 仍可能 exit 0）；
6. 再次 set 必须幂等成功；
7. 输出 `q3n markbad smoke passed`。

- [x] **Step 6: GREEN 验证**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/apply-linux-overlay.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/run-qemu.sh --fresh-nand
```

Guest:

```sh
modprobe qemu_3dnand_test
/etc/profile.d/mtd.sh q3n-markbad-smoke
```

Expected: KUnit fail 0，markbad smoke pass。

- [ ] **Step 7: 提交**

```bash
git add linux rootfs scripts/build-rootfs.sh tests/test_scripts.sh
git commit -m "feat: persist MTD bad-block markers"
```

---

### Task 5: 两次 QEMU 启动验收、文档和完整回归

**Files:**
- Create: `scripts/q3n-persistence-smoke.sh`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `rootfs/init`
- Modify: `tests/test_scripts.sh`
- Modify: `README.md`
- Modify: `qemu/README.md`
- Modify: `docs/superpowers/plans/2026-07-11-3dnand-page-raid-driver-implementation.md`

**Interfaces:**
- Produces guest stages: `q3n-persist-prepare`、`q3n-persist-verify`。
- Produces host acceptance: `./scripts/q3n-persistence-smoke.sh`。

- [ ] **Step 1: 添加失败的命令门禁**

```sh
assert_contains rootfs/profile.d/mtd.sh 'q3n-persist-prepare'
assert_contains rootfs/profile.d/mtd.sh 'q3n-persist-verify'
assert_contains rootfs/init 'MTD_SMOKE.*q3n-persist'
assert_contains scripts/q3n-persistence-smoke.sh '--fresh-nand'
assert_contains scripts/q3n-persistence-smoke.sh 'q3n-persist-prepare'
assert_contains scripts/q3n-persistence-smoke.sh 'q3n-persist-verify'
```

- [ ] **Step 2: 运行 RED**

Run: `./scripts/smoke-test.sh`

Expected: FAIL on missing `q3n-persist-prepare`。

- [ ] **Step 3: 实现 prepare stage**

`mtd_q3n_persist_prepare()`：

- fresh block 0；
- 生成 8 个 16 KiB 全零 logical data page 并写入，使物理 frontier 到 page 9；
- 轮询 parity_written 增加；
- 对 block 1 执行 `mtd_badblock set` 并确认 get=1；
- 执行 sync/关闭 fd；
- 输出 `q3n persistence prepare passed`。

- [ ] **Step 4: 实现 verify stage**

`mtd_q3n_persist_verify()`：

- 读取 block 0 前 8 个 logical page 并与全零输入比较；
- 注入 logical page 0 loss，读取并确认 parity recovery；
- 确认 block 1 get=1，write/erase 均失败；
- 向 logical page 8 写一个 page，验证恢复的物理 frontier 允许 page 9；
- 输出 `q3n persistence verify passed`。

不要在该 stage 内运行会先 erase block 0 的既有 serial smoke；回归 smoke 在独立
fresh 镜像轮次运行。

- [ ] **Step 5: init 支持命令行 stage 并自动关机**

在 `rootfs/init` 用明确 case 选择：

```sh
case "${MTD_SMOKE:-0}" in
  q3n-persist-prepare) mtd_test=mtd_q3n_persist_prepare ;;
  q3n-persist-verify) mtd_test=mtd_q3n_persist_verify ;;
  ubifs) mtd_test=mtd_ubifs ;;
  1) mtd_test=mtd_smoke ;;
  *) mtd_test= ;;
esac
```

stage 成功时打印通过并 `poweroff -f`；失败时进入 shell，host script 依靠输出
缺少 pass marker 判定失败。

- [ ] **Step 6: 实现 host 两次启动脚本**

```sh
#!/usr/bin/env sh
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

prepare_log="$work_dir/q3n-persist-prepare.log"
verify_log="$work_dir/q3n-persist-verify.log"

"$repo_root/scripts/run-qemu.sh" --fresh-nand \
  --append "MTD_SMOKE=q3n-persist-prepare" | tee "$prepare_log"
grep -q 'q3n persistence prepare passed' "$prepare_log"

"$repo_root/scripts/run-qemu.sh" \
  --append "MTD_SMOKE=q3n-persist-verify" | tee "$verify_log"
grep -q 'q3n persistence verify passed' "$verify_log"
```

脚本最后检查镜像 magic 和 sparse 实际占用小于逻辑大小，然后打印
`q3n persistence smoke passed`。

使用可移植命令取得大小：

```sh
logical_bytes=$(wc -c < "$work_dir/media/q3n-nand.raw")
allocated_kib=$(du -k "$work_dir/media/q3n-nand.raw" | awk '{print $1}')
[ $((allocated_kib * 1024)) -lt "$logical_bytes" ]
[ "$(dd if="$work_dir/media/q3n-nand.raw" bs=8 count=1 2>/dev/null)" = Q3NMEDIA ]
```

- [ ] **Step 7: 完整构建与双启动验收**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/shell.sh ./scripts/apply-linux-overlay.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-persistence-smoke.sh
```

Expected: prepare/verify 两轮均自动 poweroff，最终输出 persistence pass。

- [ ] **Step 8: 既有完整回归**

用 `--fresh-nand` 启动 guest，依次执行：

```sh
modprobe qemu_3dnand_test
/etc/profile.d/mtd.sh q3n-cancel-barrier-smoke
/etc/profile.d/mtd.sh q3n-generation-smoke
/etc/profile.d/mtd.sh q3n-serial-smoke
```

Expected: KUnit fail 0；三项 smoke 均 pass；pending/reserved/pause 回到 0。

- [ ] **Step 9: 更新文档和总计划**

README 记录默认持久镜像、`--fresh-nand` 和两次启动命令；QEMU README 记录
物理镜像 v1 与 OOB BBM，明确 QEMU 不管理 RAID 布局。总计划勾选 persistent
controller-backed markbad，保留并行/FTL/GC 非目标状态。

- [ ] **Step 10: 提交**

```bash
git add scripts rootfs tests README.md qemu/README.md \
  docs/superpowers/plans/2026-07-11-3dnand-page-raid-driver-implementation.md
git commit -m "test: verify persistent NAND restart recovery"
```

---

## Final Acceptance Checklist

- [ ] `git diff --check` 无输出，工作区干净。
- [ ] QEMU 11.0.2 fresh build 退出 0。
- [ ] Linux 7.0.12 和 rootfs fresh build 退出 0。
- [ ] 镜像 magic/版本/几何校验和 sparse 行为通过。
- [ ] 正常 QEMU 重启后 main/OOB/page state/frontier 恢复。
- [ ] BBM 实际位于首 page `OOB[0]`，重启后仍 bad。
- [ ] QEMU 不包含 `% 8`、stripe、parity 或 generation 解释。
- [ ] Linux 重建 data-valid、parity index 和余数 7 尾部 parity。
- [ ] `_block_markbad()` 幂等，所有 owner 路径正确 cancel_end。
- [ ] bad block write/erase 拒绝，read 语义保留。
- [ ] 两次 QEMU persistence smoke 通过。
- [ ] KUnit、cancel、generation、serial smoke 全部通过。
- [ ] 未修改 MTD/UBI/UBIFS core。
- [ ] 分支复审无 Critical/Important，推送后与远端同步。
