# ONFI 5.1：2 LUN × 4 Plane 并行编程与控制器能力分析

## 1. 文档目标

本文分析基于 ONFI 5.1 设备模型实现以下 page-RAID 写入的命令组织与控制器需求：

```text
2 LUN（对应目标器件中的两个可独立调度 die）
× 4 plane per LUN
= 8 physical page slots

7 data page + 1 parity page
```

本文不把“存在两个 die、每个 die 有四个 plane”直接等同于“必然支持八页并行编程”。控制器必须先读取 ONFI 参数页，并根据器件声明的 multi-LUN 和 multi-plane 能力选择执行路径。

精确命令 opcode、plane address pairing 和状态位定义仍需与目标 NAND 的 ONFI 5.1 参数页及厂商 datasheet 交叉确认。ONFI 兼容并不意味着所有 optional command 都必须实现。

## 2. ONFI 并行层次

ONFI 使用以下层次描述 NAND：

```text
Channel / NAND bus
└── Target（通常由 CE 选择）
    ├── LUN 0
    │   ├── Plane 0
    │   ├── Plane 1
    │   ├── Plane 2
    │   └── Plane 3
    └── LUN 1
        ├── Plane 0
        ├── Plane 1
        ├── Plane 2
        └── Plane 3
```

“Die”是器件物理实现概念；在 ONFI 命令层，控制器应依据 LUN 进行并发调度。只有当两个 die 暴露为两个可独立操作的 LUN，且器件声明支持 multi-LUN/interleaved operation 时，才能在一个 LUN busy 时操作另一个 LUN。

三种并行能力不同：

| 层次 | 并行内容 | 主要限制 |
| --- | --- | --- |
| 多 Channel | 命令、地址、数据传输及内部操作均可重叠 | 需要独立物理总线和 DMA |
| 多 LUN/Die | 内部 read/program/erase 时间重叠 | 同 channel 时数据传输通常仍串行 |
| 多 Plane | 同一 LUN 内多个阵列同时 program | 地址配对和命令顺序限制严格 |

因此，`2 LUN × 4 plane` 通常表示：每个 LUN 内执行 multi-plane program，再在两个 LUN 之间交错内部 program。它不一定意味着 8 个 page 同时经过 NAND I/O 总线。

## 3. 初始化与能力发现

### 3.1 基础发现流程

控制器初始化时至少执行：

```text
Reset                         FFh
Read ID at address 20h        90h
确认返回 ONFI signature
Read Parameter Page           ECh
读取冗余参数页
校验 CRC
解析 revision、几何和 optional capabilities
```

如果参数页 CRC 无效，不能根据未校验字段启用 multi-plane 或 multi-LUN 并发。应尝试规范允许的冗余副本恢复；仍无法获得有效参数时降级为安全的单页、单 LUN 模式或拒绝初始化。

### 3.2 必须获得的参数

```text
ONFI revision
page size and spare/OOB size
pages per block
blocks per LUN
LUN count
plane address bits / planes per LUN
row and column address cycles
programs per page
interleaved operation capability/count
multi-plane program capability
enhanced status capability
supported SDR/NV-DDR timing modes
```

当前 Linux 7.0.12 的 `struct nand_onfi_params` 已包含以下基础字段：

```c
u8 lun_count;
u8 programs_per_page;
u8 interleaved_bits;
u8 interleaved_ops;
```

当前解析逻辑使用：

```c
memorg->luns_per_target = p->lun_count;
memorg->planes_per_lun = 1 << p->interleaved_bits;
```

但这些几何字段只能证明存在多个 LUN/plane，不能单独证明支持目标命令组合。驱动还必须检查 ONFI 5.1 参数页中相应的 optional command/capability 声明。

### 3.3 能力判定矩阵

| 参数页结果 | 可用执行路径 |
| --- | --- |
| 2 LUN、4 plane、支持 4-plane 和 multi-LUN | 每 LUN 一次 4-plane，两个 LUN interleave |
| 2 LUN、4 plane、只支持 2-plane | 每 LUN 拆成两个 2-plane 操作，再做 LUN interleave |
| 2 LUN、不支持 multi-plane | 每 LUN 四个普通 page program，只做 LUN interleave |
| 1 LUN、支持 4-plane | 只能做单 LUN 四 plane |
| 不支持 multi-LUN/interleave | 前一 LUN 完成后才能启动下一 LUN |

## 4. 单 LUN 四 Plane Program

### 4.1 ONFI 命令模型

普通 page program 的基础序列是：

```text
80h → Column/Row Address → Main/OOB Data → 10h
```

Multi-plane program 需要在最终 program confirm 之前依次装载多个 plane 的 page register。概念序列为：

```text
Plane 0:
    PROGRAM DATA INPUT
    address(P0)
    main0 + oob0
    MULTI-PLANE INTERMEDIATE CONFIRM

Plane 1:
    MULTI-PLANE DATA INPUT
    address(P1)
    main1 + oob1
    MULTI-PLANE INTERMEDIATE CONFIRM

Plane 2:
    MULTI-PLANE DATA INPUT
    address(P2)
    main2 + oob2
    MULTI-PLANE INTERMEDIATE CONFIRM

Plane 3:
    MULTI-PLANE DATA INPUT
    address(P3)
    main3 + oob3
    FINAL PROGRAM CONFIRM
```

常见 opcode 表达为：

```text
80h + Addr(P0) + Data0 + 11h
81h + Addr(P1) + Data1 + 11h
81h + Addr(P2) + Data2 + 11h
81h + Addr(P3) + Data3 + 10h
```

其中常见含义为：

| Opcode | 常见作用 |
| --- | --- |
| 80h | Program Page Setup / Data Input |
| 81h | Multi-plane Data Input |
| 11h | Multi-plane Intermediate Confirm |
| 10h | Final Program Confirm |

上表用于描述控制器状态机，不应在 page-RAID 层硬编码为所有 ONFI 5.1 器件的无条件固定序列。控制器后端必须根据器件声明和 datasheet 确认 81h/11h 的使用方式、允许重复次数以及是否真正支持四 plane。

### 4.2 地址约束

同一 multi-plane program 的成员至少应满足：

```text
LUN(P0) = LUN(P1) = LUN(P2) = LUN(P3)

page_in_block(P0)
  = page_in_block(P1)
  = page_in_block(P2)
  = page_in_block(P3)
```

block address 通常还需满足：

```text
除 plane-select bits 外，其他 block address bits 符合器件规定的配对关系
plane-select bits 分别选择 Plane 0..3
```

典型物理位置：

```text
LUN0 / Plane0 / BlockGroupX / PageY
LUN0 / Plane1 / BlockGroupX / PageY
LUN0 / Plane2 / BlockGroupX / PageY
LUN0 / Plane3 / BlockGroupX / PageY
```

控制器必须通过解析后的 row address layout 生成地址，不能简单把 plane 编号加到线性 page number。

### 4.3 只有 2-plane 能力时

即使参数页显示每 LUN 有 4 个 plane，器件也可能只允许一次操作两个 plane。此时必须拆分：

```text
Operation A: Plane 0 + Plane 1
Operation B: Plane 2 + Plane 3
```

拆分后单 LUN 内不能视为一次四页原子操作，page-RAID 层必须聚合两次操作的状态。

## 5. 双 LUN/Die 交错编程

### 5.1 执行原理

LUN0 完成数据装载并收到 final confirm 后进入内部 program busy。若设备声明支持 multi-LUN/interleaved operation，控制器可以切换到 LUN1，装载并启动另一组 program：

```text
Time ------------------------------------------------------------>

Bus:
  [Load LUN0 P0..P3]
                       [Load LUN1 P0..P3]
                                            [Status LUN0/LUN1]

LUN0:
  [Data load..........][Internal program.........................]

LUN1:
                       [Data load..........][Internal program....]
```

同一 channel 下，两个 LUN 的数据装载通常仍然串行；真正重叠的是 NAND 内部 program 时间。如果两个 LUN 位于不同 channel，命令和数据阶段也可以并行。

### 5.2 概念命令顺序

```text
select/address LUN0
execute LUN0 four-plane data-input sequence
send LUN0 final confirm

while LUN0 is internally busy:
    select/address LUN1
    execute LUN1 four-plane data-input sequence
    send LUN1 final confirm

read status for LUN0
read status for LUN1
aggregate completion
```

面向指定 LUN 的增强状态读取常见命令模型为：

```text
78h + Row Address
```

row address 用于选择目标 LUN/plane context。最终的 address cycles、返回位含义及能否获得 plane 级状态必须以器件能力声明为准。

普通 `70h Read Status` 通常不足以可靠管理两个同时执行的 LUN，因为它可能只反映当前选中的 LUN或 target 级状态。

### 5.3 不能假设跨 LUN 原子

以下结果都必须被控制器和 page-RAID 层正确处理：

```text
LUN0 success, LUN1 success
LUN0 success, LUN1 fail
LUN0 fail,    LUN1 success
LUN0 timeout, LUN1 unknown
power loss during either LUN operation
```

ONFI multi-LUN/interleaving 提供性能并行，不提供 8-page 掉电原子性。

## 6. 7D+1P Page-RAID 映射

推荐映射：

```text
LUN 0:
    Plane 0 = D0
    Plane 1 = D1
    Plane 2 = D2
    Plane 3 = D3

LUN 1:
    Plane 0 = D4
    Plane 1 = D5
    Plane 2 = D6
    Plane 3 = Parity + OOB manifest
```

写入流程：

```text
prepare D0..D6
calculate P = D0 ^ D1 ^ ... ^ D6
build parity OOB manifest with D0..D6 CRCs

submit LUN0 four-plane program for D0..D3
submit LUN1 four-plane program for D4..D6 + P

wait and read LUN0 status
wait and read LUN1 status

if both operations succeeded:
    return MTD write success
else:
    mark whole 7D+1P stripe invalid
```

由于可能出现 parity plane 成功但某个 data plane/LUN 失败，重启后不能只根据 parity page 存在判断 stripe committed。parity OOB manifest 是候选提交凭证；驱动需要验证 7 个 data page 的 CRC 后才把 stripe 标记为有效。

## 7. 控制器必需能力

### 7.1 ONFI 发现与配置

- 支持 FFh Reset、90h Read ID、ECh Read Parameter Page。
- 校验参数页 CRC，并处理冗余参数页。
- 解析 ONFI 5.1 revision 和扩展能力信息。
- 解析 LUN、plane、地址周期和并发限制。
- 配置参数页声明的 SDR/NV-DDR timing mode。
- capability 不足时能降级到 2-plane 或普通 page program。

### 7.2 Multi-plane 命令状态机

- 支持器件声明的 multi-plane setup/intermediate/final confirm 序列。
- 在 final confirm 前保持所有 plane 的 page-register 数据。
- 检查 plane bitmap、相同 page index及 block pairing。
- 支持 main 和 OOB 在首次 page program 中一起写入。
- 支持四 plane；或显式拆为两个 2-plane operation。
- 对中间命令、数据装载和最终确认分别设置 timeout/error path。

### 7.3 Multi-LUN 调度

每个 LUN至少维护：

```c
enum q3n_lun_state {
    Q3N_LUN_IDLE,
    Q3N_LUN_LOADING,
    Q3N_LUN_PROGRAMMING,
    Q3N_LUN_DONE,
    Q3N_LUN_FAILED,
};
```

调度器必须支持：

- LUN0 busy 时向 LUN1 发送命令。
- 每个 LUN 独立的状态、timeout 和完成事件。
- 面向指定 LUN 的 status 请求。
- 两个 LUN 完成结果的聚合。
- reset/error recovery 对其他 LUN 状态的影响建模。
- 器件不支持 multi-LUN 时自动串行化。

如果控制器只有一个全局 busy 和一个全局 status，无法正确实现 ONFI multi-LUN interleave。

### 7.4 Page Buffer 和 DMA

16KiB page 的 7D+1P 至少需要：

```text
8 × 16KiB main data = 128KiB
8 × OOB
```

控制器需要：

- 8 个 page buffer，或容量等价且不会被切换 LUN 覆盖的 SRAM。
- 8 个 DMA/SG descriptor 或可连续装载八页的 descriptor ring。
- 每页独立的 main/OOB 指针和长度。
- parity main 和 OOB manifest 同次装载。
- 区分 DMA/data-load complete 与 NAND internal-program complete。
- 适用于当前 interface timing mode 的 FIFO 和流控。

### 7.5 XOR 和 Manifest

最小实现允许驱动软件计算 XOR：

```text
P = D0 ^ D1 ^ ... ^ D6
```

性能实现可以增加 streaming XOR 或 DMA XOR engine，但必须在 parity page 被装载前完成 parity 和 manifest。

parity OOB manifest 至少包含：

```text
magic
format_version
group_seq
member_bitmap = 0xff
data_crc[7]
parity_crc
header_crc
```

该设计只要求首次 program 时同时写 main 和 OOB，不要求 OOB-only，也不二次编程前面的 data page。

### 7.6 ECC、状态和错误定位

- 8 个物理 page 独立执行 ECC。
- 保存每页 corrected/uncorrectable 状态。
- 能区分 LUN0 和 LUN1 的 program status。
- 如果支持 plane-level enhanced status，保存 plane success/fail bitmap。
- 如果只能获得 LUN-level fail，则该 LUN 的四个 page 全部视为失败。
- 任意成员失败都导致整个 7D+1P stripe 对 MTD 不可见。

建议请求状态：

```c
struct q3n_raid_program {
    u8 submitted_luns;
    u8 completed_luns;
    u8 failed_luns;
    u8 submitted_planes[2];
    u8 completed_planes[2];
    u8 failed_planes[2];
};
```

成功条件：

```text
completed_luns == 0b11
failed_luns == 0
expected plane members all completed
```

## 8. 推荐控制器接口

page-RAID 层不应直接拼接 ONFI opcode。建议分层接口：

```c
struct q3n_page_desc {
    u8 lun;
    u8 plane;
    u32 block;
    u32 page;
    const void *main;
    const void *oob;
};

int q3n_prepare_page(struct q3n_page_desc *page);

int q3n_submit_multiplane(struct q3n_ctrl *ctrl,
                          u8 lun,
                          u8 plane_bitmap,
                          struct q3n_page_desc *pages);

int q3n_wait_lun(struct q3n_ctrl *ctrl, u8 lun,
                 struct q3n_lun_status *status);

int q3n_submit_raid_stripe(struct q3n_ctrl *ctrl,
                           struct q3n_page_desc pages[8]);
```

职责边界：

| 层 | 职责 |
| --- | --- |
| page-RAID | 7D+1P 布局、XOR、manifest、整组成功判定 |
| ONFI backend | capability、地址约束、opcode 序列、status 命令 |
| controller/DMA | buffer、descriptor、IRQ、timeout、总线时序 |
| NAND media | 实际 program/ECC/bad block 行为 |

## 9. 不修改 MTD Core 的 Direct MTD 驱动架构

### 9.1 设计边界

本方案不修改以下内核源码：

```text
include/linux/mtd/mtd.h
drivers/mtd/mtdcore.c
drivers/mtd/nand/raw/*
```

驱动直接初始化 `struct mtd_info`，注册 MTD 回调：

```c
mtd->priv = q3n;
mtd->_read = q3n_mtd_read;
mtd->_write = q3n_mtd_write;
mtd->_erase = q3n_mtd_erase;
mtd->_sync = q3n_mtd_sync;
mtd->_block_isbad = q3n_mtd_block_isbad;
mtd->_block_markbad = q3n_mtd_block_markbad;

ret = mtd_device_register(mtd, partitions, nr_parts);
```

MTD 回调对上保持同步语义：`_write()` 只有在两个 LUN 的 8 个 page 全部完成后才返回。驱动内部允许 asynchronous submit + completion wait，以实现 LUN0 和 LUN1 的内部 program 重叠。

该架构只解决“不修改 MTD core 时如何实现并行访问”。如果并行 profile 上报 `mtd.writesize = 112KiB`，原生 UBI/UBIFS 仍会遇到非 2 次幂限制；驱动注册钩子不能绕过 UBI/UBIFS 自己的检查。首版应由专用 MTD 用户提交完整 stripe，或者暂不挂载 UBI。

### 9.2 分层架构

```text
MTD caller
   |
   | mtd_read/write/erase/sync
   v
+-----------------------------------------+
| q3n direct MTD frontend                 |
| alignment / range / retlen / errno      |
+-----------------------------------------+
   |
   v
+-----------------------------------------+
| logical stripe layer                    |
| 112KiB <-> 7 data pages + parity        |
| XOR / manifest / CRC / recovery         |
+-----------------------------------------+
   |
   v
+-----------------------------------------+
| physical mapper                         |
| LEB/stripe -> LUN/plane/block/page       |
| plane pairing and bad-group validation  |
+-----------------------------------------+
   |
   v
+-----------------------------------------+
| ONFI scheduler                          |
| 4-plane submit per LUN                   |
| 2-LUN interleave / status aggregation   |
+-----------------------------------------+
   |
   v
+-----------------------------------------+
| controller transport                    |
| descriptor ring / DMA / MMIO / IRQ      |
+-----------------------------------------+
   |
   v
ONFI 5.1 NAND
```

各层职责：

| 模块 | 输入 | 输出 | 不负责 |
| --- | --- | --- | --- |
| MTD frontend | 逻辑 offset/length/buffer | MTD errno 和 retlen | ONFI opcode |
| stripe layer | 112KiB 逻辑 stripe | 8 个 page payload/manifest | MMIO/DMA |
| mapper | logical stripe index | 8 个物理地址 | XOR/ECC |
| ONFI scheduler | 两组 4-plane descriptor | LUN/plane completion bitmap | MTD 地址验证 |
| transport | ONFI request | DMA/IRQ completion | RAID 有效性判断 |

### 9.3 MTD 可见几何

并行 7D+1P profile 建议注册：

```c
mtd->name = "qemu-3dnand-page-raid";
mtd->type = MTD_NANDFLASH;
mtd->flags = MTD_CAP_NANDFLASH;

mtd->writesize = 7 * physical_page_size;       /* 112KiB */
mtd->writebufsize = mtd->writesize;
mtd->erasesize = 7 * pages_per_block * physical_page_size;
mtd->size = logical_block_group_count * mtd->erasesize;
```

对于 `16KiB page × 1600 pages/block`：

```text
writesize = 112KiB
erasesize = 175MiB
```

驱动内部不得使用 `IS_ALIGNED(value, writesize)` 检查 112KiB，因为该宏的底层按位对齐语义只适用于 2 的幂。应使用普通取模 helper：

```c
static bool q3n_is_aligned(u64 value, u32 unit)
{
    return unit && value % unit == 0;
}
```

物理 OOB 只用于 parity manifest 和控制器/ECC 元数据，不直接映射成一个有明确语义的 112KiB 逻辑 OOB。首版建议：

- 不注册 `_read_oob/_write_oob`。
- 不允许上层修改 parity manifest。
- 根据 MTD 消费者要求将 `oobsize/oobavail` 设为 0，或只报告逻辑只读 OOB；不能直接报告单个 16KiB 物理页的 OOB 大小。

### 9.4 驱动核心对象

```c
struct q3n_geometry {
    u32 page_size;
    u32 oob_size;
    u32 pages_per_block;
    u32 blocks_per_lun;
    u8 lun_count;
    u8 planes_per_lun;
    u8 max_multiplane_pages;
    bool multi_lun;
    bool enhanced_status;
};

struct q3n_phys_addr {
    u8 lun;
    u8 plane;
    u32 block;
    u32 page;
};

struct q3n_page_io {
    struct q3n_phys_addr addr;
    const void *main;
    const void *oob;
    dma_addr_t main_dma;
    dma_addr_t oob_dma;
    int status;
    u32 bitflips;
};

struct q3n_lun_req {
    u8 lun;
    u8 plane_bitmap;
    enum q3n_lun_state state;
    struct q3n_page_io pages[4];
    struct completion done;
    int status;
};

struct q3n_stripe_req {
    u64 logical_stripe;
    struct q3n_lun_req lun_req[2];
    u8 completed_luns;
    u8 failed_luns;
    void *parity_buf;
    void *manifest_buf;
    int status;
};

struct q3n_device {
    struct device *dev;
    struct mtd_info mtd;
    struct q3n_geometry geo;
    const struct q3n_onfi_ops *onfi_ops;
    struct q3n_hw_queue hwq;
    struct mutex admin_lock;
    struct rw_semaphore map_lock;
    mempool_t *stripe_pool;
    atomic_t inflight;
    wait_queue_head_t drain_wq;
    bool suspending;
};
```

不要使用一个全局 mutex 覆盖“准备请求、提交两个 LUN、等待完成”的整个过程。当前驱动的全局 `q3n->lock` 会把所有物理操作串行化。推荐锁范围：

- `admin_lock`：probe/remove/suspend/reset/configuration。
- `map_lock`：坏块和逻辑 block-group 映射。
- controller queue spinlock：只保护 descriptor producer/consumer index。
- 每个 request 独立 completion 和状态。
- 不在持有 spinlock 时等待 NAND ready。

### 9.5 MTD 回调接口

#### 9.5.1 `_write()`：完整并行 stripe 写入

```c
static int q3n_mtd_write(struct mtd_info *mtd, loff_t to,
                         size_t len, size_t *retlen,
                         const u8 *buf);
```

职责：

1. 检查 `to + len <= mtd->size`，并防止整数溢出。
2. 要求 `to % writesize == 0`、`len % writesize == 0`。
3. 每次取一个 112KiB 逻辑 stripe。
4. 计算 16KiB parity 和 `data_crc[7]`。
5. 构造 parity OOB manifest。
6. 映射成 LUN0/D0..D3 和 LUN1/D4..D6/P。
7. 先准备两个 LUN 的 DMA descriptor，再发布请求，避免一个 LUN 已启动而另一个在内存分配时失败。
8. 调用 `q3n_submit_stripe()`。
9. 等待两个 LUN 完成并聚合状态。
10. 只有完整 stripe 成功才增加 `*retlen`。

建议内部接口：

```c
int q3n_build_program_req(struct q3n_device *q3n,
                          u64 stripe, const u8 *data,
                          struct q3n_stripe_req *req);

int q3n_submit_stripe(struct q3n_device *q3n,
                      struct q3n_stripe_req *req);

int q3n_wait_stripe(struct q3n_device *q3n,
                    struct q3n_stripe_req *req);
```

错误返回建议：

| 场景 | errno |
| --- | --- |
| 未按 112KiB 对齐 | `-EINVAL` |
| 地址越界 | `-EINVAL` |
| request/DMA 资源不足 | `-ENOMEM` 或 `-EBUSY` |
| program fail | `-EIO` |
| controller timeout | `-ETIMEDOUT` |
| 设备正在 suspend/remove | `-EBUSY` 或 `-ENODEV` |

MTD 同一次 `_write()` 包含多个 stripe 时，前面的 stripe 成功、后续失败是允许的：`*retlen` 只报告已经完整提交的逻辑字节数。

#### 9.5.2 `_read()`：普通读取与 RAID 恢复

```c
static int q3n_mtd_read(struct mtd_info *mtd, loff_t from,
                        size_t len, size_t *retlen, u8 *buf);
```

读取可以支持非 112KiB 对齐请求，以保持 MTD 常见读取语义：

1. 根据 `from/len` 找出涉及的逻辑 stripe 和 data slot。
2. 正常路径只读取请求覆盖的 data page，不读取 parity。
3. data page ECC 不可纠时，读取同 stripe 的其余 6 个 data page和 parity。
4. 验证 parity manifest、成员 CRC 和 group identity。
5. XOR 恢复目标 page。
6. 返回最大 bitflip 统计；达到阈值时可返回 `-EUCLEAN`。
7. 无法恢复时返回 `-EBADMSG`。

内部接口：

```c
int q3n_read_phys_page(struct q3n_device *q3n,
                       const struct q3n_phys_addr *addr,
                       void *main, void *oob,
                       struct q3n_ecc_result *ecc);

int q3n_recover_data_page(struct q3n_device *q3n,
                           u64 stripe, u8 missing_slot,
                           void *recovered);
```

首次访问一个并行写 stripe 时，可以执行 manifest 的懒验证，并把结果缓存到 RAM bitmap；该缓存不是持久化提交状态。

#### 9.5.3 `_erase()`：逻辑 block-group 擦除

```c
static int q3n_mtd_erase(struct mtd_info *mtd,
                         struct erase_info *instr);
```

职责：

1. 使用取模检查 `addr/len` 按 175MiB 逻辑 eraseblock 对齐。
2. 一个逻辑 eraseblock 映射到两个 LUN、每 LUN 四个 plane 的八个物理 block。
3. 根据器件能力使用 multi-plane erase，并在两个 LUN 之间 interleave。
4. 八个物理 block 全部成功后才认为逻辑 eraseblock 擦除成功。
5. 失败时设置 `instr->fail_addr`，并将整个 block-group 标为不可用或坏。
6. 清除该组的 RAM manifest-validation bitmap。

内部接口：

```c
int q3n_submit_erase_group(struct q3n_device *q3n,
                           u32 logical_eb,
                           struct q3n_stripe_req *req);
```

#### 9.5.4 `_sync()`：排空内部请求

```c
static void q3n_mtd_sync(struct mtd_info *mtd);
```

`_sync()` 必须等待：

- 所有已提交 DMA 完成。
- 所有 LUN internal program/erase 完成。
- 所有 status 读取和 request completion 完成。

不能只等待 descriptor 已被硬件取走。建议：

```c
wait_event(q3n->drain_wq, atomic_read(&q3n->inflight) == 0);
```

首版不在 `_sync()` 中补写不完整 stripe；完整 stripe 是 `_write()` 的最小提交单位。

#### 9.5.5 坏块接口

```c
static int q3n_mtd_block_isbad(struct mtd_info *mtd, loff_t ofs);
static int q3n_mtd_block_markbad(struct mtd_info *mtd, loff_t ofs);
```

一个逻辑 eraseblock 对应八个物理 block：

- 任一成员是 factory/runtime bad，`_block_isbad()` 返回 1。
- `_block_markbad()` 将逻辑组标坏；最低成本实现至少标记当前失败成员并在驱动元数据中屏蔽整个组。
- 不实现 FTL 时不尝试自动寻找替代 block-group。
- bad-block 元数据必须在 probe 时重建，不能只保存在 RAM。

如果控制器/QEMU 模型暂不支持持久化坏块标记，应显式返回 `-EOPNOTSUPP`，不能注册一个重启后丢失状态的伪实现。

#### 9.5.6 可选 `_writev()`

```c
static int q3n_mtd_writev(struct mtd_info *mtd,
                          const struct kvec *vecs,
                          unsigned long count,
                          loff_t to, size_t *retlen);
```

实现 `_writev()` 可以直接把 7 个数据片段映射为 7 个 physical page descriptor，减少拼接 112KiB 连续 buffer 的 memcpy。要求所有 iovec 合计长度是 writesize 的整数倍，且 DMA 层支持 scatter-gather。

这只是性能优化，不应作为首版必需接口。

#### 9.5.7 suspend/resume/remove

可选注册：

```c
mtd->_suspend = q3n_mtd_suspend;
mtd->_resume = q3n_mtd_resume;
mtd->_reboot = q3n_mtd_reboot;
```

共同流程：

1. 阻止新请求进入。
2. 调用内部 drain，等待 inflight 为 0。
3. 保存或重新读取 controller configuration。
4. resume 时重新验证 ONFI timing/capability 和中断状态。
5. remove 时先 `mtd_device_unregister()`，再释放 IRQ/DMA/request pool。

### 9.6 ONFI 后端接口

page-RAID 和 MTD frontend 不直接拼接 opcode。建议：

```c
struct q3n_onfi_ops {
    int (*discover)(struct q3n_device *q3n,
                    struct q3n_geometry *geo);

    int (*prepare_multiplane)(struct q3n_device *q3n,
                              struct q3n_lun_req *req);

    int (*submit_lun)(struct q3n_device *q3n,
                      struct q3n_lun_req *req);

    int (*read_lun_status)(struct q3n_device *q3n, u8 lun,
                           struct q3n_lun_status *status);

    int (*read_page)(struct q3n_device *q3n,
                     struct q3n_page_io *page);

    int (*erase_multiplane)(struct q3n_device *q3n,
                            struct q3n_lun_req *req);

    int (*reset_lun)(struct q3n_device *q3n, u8 lun);
};
```

`prepare_multiplane()` 负责验证 4-plane 或降级成两个 2-plane command group；`submit_lun()` 负责实际的 80h/81h/11h/10h 序列。这样 opcode 变化不会传播到 MTD 或 RAID 层。

### 9.7 Controller Transport 和 IRQ

当前单页 PIO 模型：

```text
set one address
write one data window
issue one command
poll one global status
```

不足以表达两个 LUN 的并行请求。建议控制器接口至少提供：

```c
struct q3n_hw_desc {
    u8 opcode;
    u8 lun;
    u8 plane;
    u8 flags;
    u64 row;
    u32 column;
    dma_addr_t main_dma;
    dma_addr_t oob_dma;
    u32 main_len;
    u32 oob_len;
    u32 request_id;
};
```

需要的 transport 接口：

```c
int q3n_hw_queue_descs(struct q3n_hw_queue *q,
                       struct q3n_hw_desc *descs,
                       unsigned int nr);

void q3n_hw_kick(struct q3n_hw_queue *q);

irqreturn_t q3n_irq(int irq, void *data);
```

IRQ handler 只做：

1. 读取并确认 completion/error ring。
2. 根据 request_id 找到 `q3n_lun_req`。
3. 更新完成和失败 bitmap。
4. 在 LUN request 完成时调用 `complete()`。
5. 将复杂恢复、CRC 和 MTD completion 留给进程上下文。

如果硬件没有 descriptor ring，最低实现也需要两个独立 LUN context register/status slot；仅有一个地址寄存器和一个全局 READY 位无法安全并行。

### 9.8 同步 MTD 回调中的双 LUN 并行

MTD `_write()` 本身是同步函数，但内部仍可实现并行：

```c
prepare LUN0 descriptors
prepare LUN1 descriptors

submit LUN0
submit LUN1 while LUN0 is internally busy

wait_for_completion(&req->lun_req[0].done)
wait_for_completion(&req->lun_req[1].done)

aggregate status
return
```

第一阶段只允许一个 stripe request 在飞，也能获得单个 stripe 内的 2-LUN interleave 收益，代码最简单。

第二阶段才增加 queue depth，使多个调用/stripe 并发：

- 使用 mempool 预分配 request 和 parity buffer。
- 使用 semaphore 限制最大 inflight stripe。
- 使用 request_id 区分中断完成。
- 同一物理 block/page program order 由 scheduler 保证。
- erase 与同 block-group read/write 必须互斥。

不要为了队列深度在第一版引入复杂锁；先验证一个 stripe 内的双 LUN 并行。

### 9.9 注册与注销顺序

Probe：

```text
enable PCI/platform device
map MMIO and configure DMA mask
allocate IRQ and controller queues
reset controller/NAND
read and validate ONFI 5.1 parameter page
select 4-plane/2-plane/fallback capability
initialize mapper, request pool and bad-block state
fill struct mtd_info
mtd_device_register()
enable debugfs/statistics
```

Remove/error unwind：

```text
mark device removing
mtd_device_unregister()
reject new requests
drain or abort inflight requests
disable IRQ/controller DMA
free request pool and mappings
```

必须在 MTD 注册前完成 capability 和几何确认；注册后再改变 writesize、erasesize 或 size 是不允许的。

### 9.10 与当前驱动的差异

| 当前实现 | 目标实现 |
| --- | --- |
| `mtd.writesize = physical page_size` | `mtd.writesize = 7 × page_size` |
| 逐个物理 page program | 一次逻辑请求构造 8-page stripe |
| parity 写到独立 pool | parity 固定映射到 LUN1/Plane3 |
| 全局 mutex 包围整个 I/O | request completion + 短临界区 |
| MMIO PIO data window | DMA/descriptor 或至少双 LUN context |
| 单全局 READY/status | 每 LUN completion/status |
| RAM parity index | parity OOB manifest + 启动/懒验证 |
| 仅 `_read/_write/_erase` | 增加 `_sync` 和坏块钩子 |
| `IS_ALIGNED()` 检查 | 对 112KiB 使用 `%` 对齐检查 |

### 9.11 最小首版接口集合

必须实现：

```text
MTD:      _read, _write, _erase, _sync
Badblock: _block_isbad；有持久化能力时实现 _block_markbad
RAID:     map, XOR, manifest build/validate, recovery
ONFI:     discover, submit_lun, read_lun_status, read_page,
          erase_multiplane, reset_lun
HW:       prepare descriptors, queue/kick, IRQ completion, timeout
```

可以后置：

```text
_writev
多 stripe queue depth
plane-level 精细错误定位
硬件 XOR
异步读并发
debugfs 故障注入和性能统计扩展
```

### 9.12 关键测试

- 注册后检查 sysfs 中 size/writesize/erasesize 与设计一致。
- 112KiB 对齐写成功；非对齐写返回 `-EINVAL`。
- LUN0 busy 时确认 LUN1 已收到命令，而不是等待 LUN0 完成。
- `_write()` 不在两个 LUN 完成前返回。
- LUN0/LUN1 任一失败时 `retlen` 不包含该 stripe。
- `_sync()` 等待 internal program，而非只等待 DMA。
- 175MiB erase 映射到八个物理 block。
- 任一成员坏块使整个逻辑 eraseblock `_block_isbad()` 为真。
- 非对齐读取、跨 stripe 读取和 ECC RAID 恢复。
- suspend/remove 与 inflight I/O 竞态。
- 使用 lockdep、KASAN、KCSAN 检查请求生命周期和锁顺序。

## 10. 当前 Linux 7.0.12 支持缺口

### 10.1 ONFI revision 解析不足

当前工程源码 `include/linux/mtd/onfi.h` 的 revision 常量只列到 ONFI 4.0，而 `drivers/mtd/nand/raw/nand_onfi.c` 的版本选择逻辑实际上只选择到 ONFI 2.3。

因此，不能依赖当前 raw NAND ONFI parser 完整识别 ONFI 5.1 revision 和新增字段。需要：

1. 补充 ONFI 5.1 revision/capability 定义并扩展 parser；或
2. 在 `qemu_3dnand` direct MTD 驱动/控制器私有层解析所需参数页字段。

第二条更符合当前 page-RAID 原型的最小改动原则，但会形成一套驱动私有 ONFI capability parser，需要单元测试保证字段偏移、endianness 和 CRC 正确。

### 10.2 Raw NAND 通用抽象不足

当前通用 raw NAND 层主要面向单 page operation，没有为本方案直接提供完整的“4-plane × 2-LUN RAID stripe”提交接口。强行把 112KiB stripe 伪装成 raw NAND page 会引入更多 writesize 和地址假设。

推荐继续使用 direct MTD driver：

- page-RAID 层提交八个物理 page descriptor。
- 私有 ONFI backend 产生实际 multi-plane/multi-LUN 命令。
- MTD 只看到 112KiB 逻辑写入单元。
- 不修改 raw NAND 通用框架完成第一版验证。

## 11. 降级策略

控制器不能假设目标器件总有完整能力。建议按以下顺序选择：

```text
4-plane + 2-LUN interleave
        ↓ unsupported
2-plane pairs + 2-LUN interleave
        ↓ unsupported
single-plane program + 2-LUN interleave
        ↓ unsupported
fully serialized single-page program
```

无论降级到哪一级，逻辑 7D+1P 格式和 parity manifest 都可保持不变；变化的是物理 program 调度和性能。只有当器件的地址约束无法让八个成员形成稳定 stripe 时，才需要拒绝该 profile。

## 12. 验证计划

### 12.1 参数页测试

- ONFI 5.1 revision 和有效 CRC。
- 三个参数页副本中单个副本损坏。
- LUN count、plane bits 和 interleaved capability 不同组合。
- 4-plane、2-plane 和不支持 multi-plane 的降级。

### 12.2 命令序列测试

- 捕获每个 CLE/ALE/data phase，核对 intermediate/final confirm 顺序。
- 检查同一 multi-plane operation 的 LUN、page index和 block pairing。
- 验证 LUN0 busy 时是否允许启动 LUN1。
- 验证 70h 与面向 LUN 的 enhanced status 行为。

### 12.3 故障注入

- 任一 plane data-load fail。
- intermediate confirm fail。
- LUN0 success/LUN1 fail 及其反向组合。
- parity plane 成功但 data plane 失败。
- status timeout、reset 和 program busy 期间掉电。

成功标准：只有全部八个成员通过状态和 CRC 验证时，7D+1P stripe 才对 MTD 可见；任何能力不支持都走显式降级或拒绝路径，不静默使用未声明的命令。
