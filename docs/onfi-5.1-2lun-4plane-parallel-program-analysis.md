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

## 9. 当前 Linux 7.0.12 支持缺口

### 9.1 ONFI revision 解析不足

当前工程源码 `include/linux/mtd/onfi.h` 的 revision 常量只列到 ONFI 4.0，而 `drivers/mtd/nand/raw/nand_onfi.c` 的版本选择逻辑实际上只选择到 ONFI 2.3。

因此，不能依赖当前 raw NAND ONFI parser 完整识别 ONFI 5.1 revision 和新增字段。需要：

1. 补充 ONFI 5.1 revision/capability 定义并扩展 parser；或
2. 在 `qemu_3dnand` direct MTD 驱动/控制器私有层解析所需参数页字段。

第二条更符合当前 page-RAID 原型的最小改动原则，但会形成一套驱动私有 ONFI capability parser，需要单元测试保证字段偏移、endianness 和 CRC 正确。

### 9.2 Raw NAND 通用抽象不足

当前通用 raw NAND 层主要面向单 page operation，没有为本方案直接提供完整的“4-plane × 2-LUN RAID stripe”提交接口。强行把 112KiB stripe 伪装成 raw NAND page 会引入更多 writesize 和地址假设。

推荐继续使用 direct MTD driver：

- page-RAID 层提交八个物理 page descriptor。
- 私有 ONFI backend 产生实际 multi-plane/multi-LUN 命令。
- MTD 只看到 112KiB 逻辑写入单元。
- 不修改 raw NAND 通用框架完成第一版验证。

## 10. 降级策略

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

## 11. 验证计划

### 11.1 参数页测试

- ONFI 5.1 revision 和有效 CRC。
- 三个参数页副本中单个副本损坏。
- LUN count、plane bits 和 interleaved capability 不同组合。
- 4-plane、2-plane 和不支持 multi-plane 的降级。

### 11.2 命令序列测试

- 捕获每个 CLE/ALE/data phase，核对 intermediate/final confirm 顺序。
- 检查同一 multi-plane operation 的 LUN、page index和 block pairing。
- 验证 LUN0 busy 时是否允许启动 LUN1。
- 验证 70h 与面向 LUN 的 enhanced status 行为。

### 11.3 故障注入

- 任一 plane data-load fail。
- intermediate confirm fail。
- LUN0 success/LUN1 fail 及其反向组合。
- parity plane 成功但 data plane 失败。
- status timeout、reset 和 program busy 期间掉电。

成功标准：只有全部八个成员通过状态和 CRC 验证时，7D+1P stripe 才对 MTD 可见；任何能力不支持都走显式降级或拒绝路径，不静默使用未声明的命令。
