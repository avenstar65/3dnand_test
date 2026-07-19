# Q3N v2 持久化 LDPC、MTD ECC 与串行 Page-RAID 组合设计

## 1. 目标与工程基线

本设计针对 `/Users/yangyu/Documents/3dnand-page-raid-serial-worktree` 的 `codex/page-raid-serial-async-priority` 分支。现有实现已经具备：

- 同一物理 block 内 `D0..D6,P` 的串行 Page-RAID；
- 前台优先、后台 parity read/write 调度；
- main+OOB controller 命令；
- data metadata、parity manifest 和 CRC 的结构定义及 KUnit 覆盖；
- Q3NMEDIA v1 持久化物理 NAND 镜像；
- 坏块标记、program frontier、重启 replay 和故障 smoke tests。

目标是在不修改通用 MTD core、UBI 或 UBIFS 的前提下，增加位于 QEMU 层的模拟 LDPC 能力模型，显式持久化 LDPC 码，并把 clean、corrected bitflips、uncorrectable 结果正确映射到 MTD 和 Page-RAID。

## 2. 固定 LDPC Profile

首版参数固定为：

| 参数 | 值 |
| --- | ---: |
| main page | 16 KiB |
| ECC step | 1 KiB |
| steps/page | 16 |
| strength | 每 step 40 bit |
| LDPC bytes/step | 96 B |
| LDPC bytes/page | 1536 B |
| physical OOB/page | 1664 B |
| controller logical OOB/page | 128 B |
| MTD bitflip threshold | 40 bit |

本实现模拟纠错能力和结果，不实现真实 LDPC 校验矩阵、编码或迭代译码。每个 step 仍生成确定性的 96 B 模拟 LDPC 内容，使介质容量、持久化格式和校验码损坏可以真实建模。

## 3. Q3NMEDIA v2 物理布局

Q3NMEDIA 从版本 1 升级为版本 2。每个物理 page 固定为 18048 B：

```text
physical page
├── main[0..16383]                     16384 B
└── physical OOB[0..1663]              1664 B
    ├── physical OOB[0]                   1 B  BBM
    ├── physical OOB[1..1536]           1536 B  16 × 96 B LDPC
    └── physical OOB[1537..1663]         127 B  metadata
```

v2 header 明确保存 `physical_oob_size=1664`、`ldpc_bytes_per_step=96`、`ldpc_steps=16` 和 `logical_oob_size=128`。介质布局计算为：

```text
page_stride              = 16384 + 1664 = 18048
physical_oob_offset      = slot_offset + 16384
bbm_offset               = physical_oob_offset
ldpc_step_offset(step)   = physical_oob_offset + 1 + step * 96
metadata_offset          = physical_oob_offset + 1537
```

controller 向软件映射一个非连续的 128 B logical OOB：

```text
logical OOB[0]      <-> physical OOB[0]       BBM
logical OOB[1..127] <-> physical OOB[1537..1663]
```

LDPC 区 `physical OOB[1..1536]` 不进入 logical OOB 地址空间。软件读取 logical OOB 第 0 字节必须得到 BBM；软件写 logical OOB 第 0 字节必须真实编程 BBM，不设置写保护。第一页 logical OOB[0] 从 `0xff` 编程为任意非 `0xff` 值后，该 block 被识别为坏块。其他页仍按相同映射保存 OOB[0]，但坏块扫描和 block status 以第一页 BBM 为准。

data metadata 和 parity manifest 必须从 logical OOB[1] 开始，不能覆盖 BBM。现有主路径尚未真正持久化这些结构；本次实现必须把 `q3n_data_meta` 和 `q3n_parity_manifest` 接入 `PROGRAM_PAGE_OOB`/`READ_PAGE_OOB`。main-addressable MMIO 地址仍以 16 KiB page 为单位，Linux 逻辑和物理页号映射不变。

v1 镜像与 v2 的 page stride 和 OOB 语义不兼容。加载 v1 时明确返回不兼容错误并提示重新创建，不实现隐式或原地迁移。空后端自动创建 v2 镜像。

program page 的原子介质单元为 main+physical OOB。QEMU controller 根据 main、物理 page key、step 和 profile version生成LDPC，将logical OOB反向映射到BBM和metadata位置，再调用media层一次写入完整page slot。erase后page state恢复为erased；读取erased page时main和physical OOB均表现为全`0xff`。

## 4. QEMU 层职责划分

### 4.1 `q3n-media`

media 层只负责 v2 持久化布局和原始区域 I/O：

- 保存和读取 main 与 1664 B raw physical OOB；
- 校验 v2 header 和镜像长度；
- 维护 page state、program frontier 和坏块标记；
- erase block；
- 保存持久化 bitflip overlay 状态。

LDPC 是否可纠、最大 bitflips 和译码结果属于 controller 语义，不放入 media 层。

### 4.2 `q3n-nand` controller

controller 负责：

- program 时生成确定性模拟 LDPC 码并完成 logical/physical OOB 反向映射；
- read 时取得 main 和 physical OOB，再映射出 logical OOB；
- 应用 main/LDPC bitflip overlay；
- 按 step 合并统计 main 和 LDPC 区错误数；
- 返回纠正后的 main/OOB 或 uncorrectable；
- 锁存 ECC geometry 和本次 read 结果寄存器；
- 维护物理 LDPC 统计。

模拟 LDPC 内容由 main step、物理 page key、step index 和 profile version导出。验证算法不读取overlay后的main作为“原始数据”；controller先从持久化介质获得原始内容，再把overlay用于错误计数和失败时的损坏视图。模拟码缺失或与原始main不匹配时，对应step不可纠。

## 5. 持久化 bitflip overlay

采用错误覆盖层，不直接覆写 main 或 LDPC 原始内容。每个存在错误的物理页保存两个稀疏 bitmap：

```text
main overlay:  16 KiB / 8 = 2048 B
LDPC overlay: 1536 B / 8 =  192 B
```

注入参数包含：物理 main 地址、step、区域（main/LDPC）、step 内首 bit 和数量。同一 bit 再次注入等价于再次翻转，因此两次注入恢复原状态。

overlay 必须随 v2 镜像持久化，以便现有“重启后恢复”测试覆盖 ECC 场景。v2 header 增加 overlay state/slot 区的 offset 和 length；每页 overlay slot 固定 2240 B，后端继续依赖 sparse allocation，不做全量预分配。erase block 将对应 overlay slots 写零并清除内存状态。

精确边界验证采用减法式检查，拒绝 step、region、first bit、count 越界和整数溢出。

## 6. 每 step 纠错判定

对每个 1 KiB step，将 main overlay 与对应 96 B LDPC overlay 的 popcount 相加：

- 0 bit：CLEAN；
- 1..40 bit：CORRECTED；
- 41 bit 以上：UNCORRECTABLE；
- 模拟 LDPC 缺失或不匹配：UNCORRECTABLE。

一个物理页只要有一个 step 不可纠，页结果就是 UNCORRECTABLE。否则：

- `ECC_MAX_BITFLIPS` 是任一 step 的最大纠正数；
- `ECC_CORRECTED_BITS` 是全页所有 step 的纠正数之和；
- 纠正成功时 data window 返回原始正确 main/OOB；
- 不可纠时驱动不得消费 data window 中的内容。

擦除页使用隐式全 `0xff` 原始内容执行相同阈值判定，不要求存在已生成 LDPC 码。

## 7. MMIO ABI

QEMU 与 Linux 头文件新增完全一致的常量和寄存器：

```text
ECC_GEOM0             step_size | strength
ECC_GEOM1             parity_bytes=96 | steps_per_page=16
ECC_STATUS            CLEAN | CORRECTED | UNCORRECTABLE
ECC_MAX_BITFLIPS
ECC_CORRECTED_BITS
ECC_FAILED_STEP

FAULT_STEP
FAULT_REGION          MAIN | LDPC
FAULT_FIRST_BIT
FAULT_COUNT
FAULT_CTRL             INJECT_BITFLIPS
```

read命令开始时重置锁存结果，命令完成后保持到下一条命令。普通介质I/O错误继续使用`STATUS_ERROR`；LDPC不可纠使用独立status bit/result，不与普通`-EIO`混淆。

probe 读取并严格验证 profile，然后设置：

```c
mtd->ecc_step_size = 1024;
mtd->ecc_strength = 40;
mtd->bitflip_threshold = 40;
```

驱动同时实现 `_read_oob` 和 `_write_oob`，向 MTD 暴露完整 128 B logical OOB。`ooboffs=0` 对应 BBM，不能由驱动自动跳过或写保护；对 block 第一物理页的 logical OOB[0] 写入非 `0xff` 后，后续 block status 必须报告 bad。OOB 操作仍遵守 NAND 1→0、页 program 次数和顺序约束。对已经包含 driver-owned data metadata 或 parity manifest 的页执行 raw/place OOB 写入可能使 RAID metadata 失效，这是显式 raw 介质操作的结果，读取时必须通过 CRC/manifest 校验发现，不能静默接受。

## 8. 驱动内部读取结果

所有物理读取统一返回：

```c
struct q3n_ecc_result {
	u32 max_bitflips;
	u32 corrected_bits;
	u32 failed_step;
	bool uncorrectable;
};
```

普通I/O错误仍通过负errno返回。LDPC结果通过结构返回，使前台读取、后台parity rebuild、启动replay和RAID恢复可以明确选择统计策略。

统计规则：

- 前台目标页和RAID恢复来源页的实际可纠位数计入`mtd->ecc_stats.corrected`；
- 后台parity build/replay读取计入QEMU物理健康统计，但不计入某次前台MTD读取；
- 目标页LDPC失败后先尝试RAID，不立即增加`mtd->ecc_stats.failed`；
- 只有最终无法返回正确逻辑数据时，`mtd->ecc_stats.failed++`一次。

多页 `_read()` 聚合所有成功逻辑页的最大 bitflips，并在后续页最终失败时保留已完成 `retlen`。

## 9. 与串行异步 Page-RAID 的组合

现有 manifest 结构已经定义每个 data page CRC、parity CRC 和 header CRC，但主路径尚未持久化。本次实现复用该格式，将 data metadata 和 parity manifest 写入 logical OOB[1..127]；无需增加新的 RAID metadata 结构。

读取流程固定为：

```text
foreground target read
├── LDPC CLEAN/CORRECTED
│   └── return data and actual bitflips
└── LDPC UNCORRECTABLE
    ├── require PROTECTED/current manifest
    ├── read parity through LDPC
    ├── read other D members through LDPC
    ├── reject any second uncorrectable member
    ├── XOR reconstruct target
    └── compare target CRC from persistent manifest
```

组合结果：

| 场景 | MTD 语义 |
| --- | --- |
| 最大 0..39 bit/step | 成功，返回聚合 bitflips |
| 最大 40 bit/step | `-EUCLEAN` |
| 目标页不可纠、RAID 恢复且 CRC 正确 | `-EUCLEAN` |
| stripe 尚未保护或 manifest 无效 | `-EBADMSG` |
| parity 或另一 data page 不可纠 | `-EBADMSG` |
| 重建目标 CRC 不匹配 | `-EBADMSG` |

RAID恢复成功时将逻辑结果的max bitflips至少提升到`mtd->bitflip_threshold`，不虚构实际corrected bit数。`raid_recovered`增加，`ecc_stats.failed`不增加。最终失败时`raid_failed`和`ecc_stats.failed`各增加一次。

后台异步 parity worker读取D0..D6时，各页都先经过LDPC。若任一来源不可纠，则不写parity manifest，将stripe保持UNPROTECTED/FAILED；不能使用损坏数据计算parity。

## 10. 可观测性

除标准 MTD `ecc_stats` 外，QEMU MMIO 或 Linux debugfs 提供：

```text
ldpc_corrected_bits
ldpc_uncorrectable_pages
ldpc_failed_steps
raid_recovered
raid_failed
raid_source_corrected_bits
background_ecc_corrected_bits
faults_injected
```

`ldpc_uncorrectable_pages`记录物理失败事件，包括随后被RAID恢复的页；`mtd->ecc_stats.failed`只记录最终逻辑读取失败。

guest debugfs注入格式固定为：

```text
<physical-main-byte-address> <step> <main|ldpc> <first-bit> <count>
```

## 11. 验证矩阵

必须覆盖：

1. v1 image 被明确拒绝，空 image 创建 v2。
2. v2 header、18048 B page stride、LDPC/metadata/BBM offset 正确。
3. logical OOB[0]读回第一页BBM，写为非`0xff`后block status变为bad。
4. logical OOB[1..127]与physical OOB[1537..1663]双向映射正确，LDPC区不可见。
5. 39 bit：数据纠正、无 `-EUCLEAN`、corrected 增加 39。
6. 40 bit：数据纠正并形成 `-EUCLEAN`。
7. 41 bit、stripe未保护：`-EBADMSG`。
8. 两个step各30 bit：总corrected 60、max bitflips 30。
9. 同step main 25 bit + LDPC 16 bit：合计41，不可纠。
10. 目标41 bit、stripe已保护：CRC验证后RAID恢复，形成`-EUCLEAN`。
11. 目标与另一data成员各41 bit：`-EBADMSG`。
12. 目标与parity各41 bit：`-EBADMSG`。
13. 重建CRC错误：`-EBADMSG`，不泄露错误数据。
14. 后台parity build遇到不可纠成员：不发布manifest，stripe不进入PROTECTED。
15. erase清除main/LDPC overlay并保持OOB坏块规则。
16. 重启后LDPC区、overlay、ECC结果和RAID恢复行为保持一致。
17. 多页读取后续失败：保留此前`retlen`并返回`-EBADMSG`。
18. QEMU和Linux ABI静态一致，QEMU、kernel、rootfs构建及guest smoke全部通过。

## 12. 非目标

- 不实现真实LDPC矩阵、编码器或迭代译码器。
- 不向MTD OOB API暴露controller LDPC区。
- 不迁移Q3NMEDIA v1镜像。
- 不修改通用MTD core、UBI或UBIFS。
- 不支持同一stripe两个及以上不可纠成员。
- 不引入FTL、GC、磨损均衡或自动物理重写。
