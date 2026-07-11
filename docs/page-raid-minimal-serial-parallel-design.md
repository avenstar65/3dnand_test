# 3D NAND Page-RAID 最小化设计：串行与并行方案

## 1. 目标与基线

本文基于当前工程使用的 Linux 7.0.12，设计不依赖 FTL、GC、磨损均衡和动态重映射的 page-RAID。目标是用最小代价实现单页失效恢复，并分析 MTD、UBI 以及必要时 UBIFS 的修改范围。

设计包含两种 profile：

- 串行方案：每 N 个 data page 增加一个 parity page，N 个数据页全部写完后计算并写 parity。
- 并行方案：利用双 plane、每 plane 4 个 page slot 的几何，一次提交 7 data page + 1 parity page；MTD 逻辑写入粒度为 7 × physical_page_size。

共同约束：

- parity 使用 XOR，只恢复 stripe 内一个失败 page。
- 不覆盖已经编程的 page，不回收 parity page。
- 掉电后能识别未完成 stripe，但不自动补写。
- 所有内部几何使用普通整数，通过除法和取模映射，不假设 2 的幂。
- 首版不支持任意小写原子性，完整 stripe 提交后才有效。

## 2. 非目标

- 不实现 FTL、GC、磨损均衡、热冷数据分离。
- 不恢复同一 stripe 内两个及以上 page 失效。
- 不在已写 page 上原地更新 parity。
- 不保证掉电后恢复未提交 stripe 的数据。
- 不为兼容通用 MTD 小写而引入持久化 staging log；该机制本质上会重新引入日志/FTL 类复杂度。

## 3. 总体架构

```text
UBIFS / UBI raw volume / MTD user
                 |
              MTD core
                 |
        qemu_3dnand direct MTD driver
        +-----------------------+
        | request validation    |
        | logical mapper        |
        | serial/parallel path  |
        | XOR and recovery      |
        | parity validator      |
        +-----------------------+
                 |
       NAND controller / QEMU model
                 |
            physical NAND
```

推荐“公共 RAID 核心 + 两条独立写路径”：

| 模块 | 职责 |
| --- | --- |
| geometry | 保存普通整数几何，检查容量和整除边界 |
| mapper | 逻辑地址到 block、stripe、slot、plane、page 的映射 |
| XOR engine | 计算 parity，重建单个缺失 page |
| serial writer | 完成 N 个 data page 后写 parity |
| parallel writer | 组合 7D+1P，提交 multi-plane 命令 |
| parity validator | 根据 parity manifest 识别 committed、incomplete、corrupt stripe |
| recovery | ECC 不可纠时读取其余成员并恢复 |

## 4. Parity Page 内嵌提交信息

本方案不使用独立 commit page，也不要求控制器支持 OOB-only program。提交信息只存放在 parity page 的 OOB 中，并与 parity main data 在该页首次编程时一次写入：

```text
program_page(parity_main, parity_oob_manifest)
```

控制器需要支持常规的 main + OOB 同次 page program，但不需要在 parity page 写完后再次修改 OOB，也不会重复编程前面的 data page。

parity OOB manifest 保存：

| 字段 | 建议宽度 | 说明 |
| --- | ---: | --- |
| magic | 16 bit | page-RAID 标识 |
| format_version | 8 bit | 布局版本 |
| profile | 8 bit | serial-N+1 或 parallel-7+1 |
| group_seq | 64 bit | stripe 序号 |
| role | 8 bit | 固定为 parity/manifest |
| member_bitmap | 16 bit | 预期成员集合 |
| data_crc[N] | N × 32 bit | 每个 data page 的 CRC |
| parity_crc | 32 bit | parity main data 的 CRC |
| header_crc | 32 bit | 元数据校验 |

如果 parity OOB 空间不足，可将 manifest 放在控制器提供的受保护 metadata/spare 区。只有确认 parity main data 可以保留少量非校验字节时，才考虑占用 main 区；否则不能牺牲 parity 覆盖范围。

parity page 同时承担两个角色：

```text
parity main = D0 ^ D1 ^ ... ^ D(N-1)
parity OOB  = stripe manifest + commit evidence
```

有效性规则：

1. parity page 不存在或其 ECC、parity_crc、header_crc 无效：INCOMPLETE。
2. 串行方案中，parity manifest 有效即表示前面的 data page 写流程已完成；读取时仍可用 data_crc 验证成员。
3. 并行方案中，parity manifest 只表示候选提交；必须确认 D0..D6 均可读且 data_crc 全部匹配后才是 COMMITTED。
4. parity manifest 有效但任一成员缺失或 CRC 不匹配：CORRUPT，返回 -EBADMSG。
5. data 和 parity 均为全 0xff：EMPTY。

并行方案不能仅凭“parity page 存在”判断提交，因为一次 multi-plane program 可能出现 parity 成功但某个 data slot 失败。成员 CRC 验证用于消除这种误判。

## 5. 串行方案：N+1

### 5.1 布局和映射

```text
stripe 0: D0 D1 ... D(N-1) P0
stripe 1: D0 D1 ... D(N-1) P1
```

```text
logical_page       = logical_offset / physical_page_size
page_in_eraseblock = logical_page % data_pages_per_eraseblock
stripe             = page_in_eraseblock / N
slot               = page_in_eraseblock % N
physical_page      = stripe * (N + 1) + slot
parity_page        = stripe * (N + 1) + N
```

如果 pages_per_block 不能被 N+1 整除，允许 block 尾部留下 unused pages；首版不做跨 block stripe。

### 5.2 写入时序

1. 验证请求按完整 stripe 对齐。
2. 依次写 D0 到 D(N-1)，同时累计 XOR。
3. 最后一个 data page 成功后写 parity。
4. 最后一次性写入 parity main data 和 parity OOB manifest。
5. parity page ECC、parity CRC 和 manifest CRC 有效后整组生效。

推荐严格模式：一次请求必须包含 N × page_size 数据，驱动不保留跨请求脏缓存。

兼容模式可让 mtd.writesize 保持物理 page_size，并缓存 N 个独立写请求，但前 N-1 次写返回成功时 stripe 尚未提交。除非再增加持久化 staging area，否则这不满足严格同步写语义，因此不建议用于 UBI。

### 5.3 读恢复

正常读取只访问目标 data page。ECC 不可纠时：

1. 确认 parity manifest 有效。
2. 读取 parity 和其余 N-1 个 data page。
3. 只有一个成员失败时执行 XOR 恢复。
4. 校验 parity manifest 中对应的 data_crc；成功则返回数据，失败返回 -EBADMSG。

### 5.4 代价

| 项目 | 代价 |
| --- | --- |
| 容量 | parity 基本开销为 1/(N+1)，无独立 commit page |
| 写放大 | (N+1)/N，无额外 commit page program |
| RAM | 严格流式 XOR 约 page_size；整组缓存为 N × page_size |
| 延迟 | N 个 data program + 1 个 parity program，基本串行 |
| 实现复杂度 | 低；但完整 stripe 写粒度不兼容通用 UBI 小写 |

## 6. 并行方案：7+1

### 6.1 几何和布局

```text
parallel stripe slots 0..6 = D0..D6
parallel stripe slot 7     = P

logical payload = 7 * physical_page_size
physical write  = 8 * physical_page_size
```

物理 page 为 16KiB 时，逻辑 stripe 为 112KiB，物理 program 总量为 128KiB。

### 6.2 写入时序

1. 上层提交一个完整 112KiB 逻辑单元。
2. 驱动计算 7 个 16KiB data page 的 16KiB parity。
3. 构造包含 8 个 page descriptor 的 multi-plane 命令。
4. 控制器并行 program 8 个 slot。
5. parity main data 和包含成员 CRC 的 OOB manifest 与 parity page 同时编程。
6. 命令完成时只有 8 个 slot 全部成功才向本次调用返回成功。
7. 重启后将有效 parity manifest 视为候选提交；读取并验证 D0..D6 的 CRC 后才标记 COMMITTED。

为避免启动时读取全部数据页，推荐懒验证：启动扫描只建立 parity 候选索引，第一次访问 stripe 时读取并校验 7 个 data page，成功后在 RAM bitmap 中缓存 COMMITTED 状态。重启后重新验证，不把 RAM 状态持久化。

### 6.3 地址映射

```text
stripe_index     = logical_offset / (7 * page_size)
offset_in_stripe = logical_offset % (7 * page_size)
data_slot        = offset_in_stripe / page_size
column           = offset_in_stripe % page_size
page_in_block    = stripe_index % pages_per_block
block_group      = stripe_index / pages_per_block
parity_slot      = 7
```

逻辑 eraseblock：

```text
mtd_erasesize = 7 * pages_per_block * page_size
```

按当前示例几何 1600 pages/block、16KiB/page：

```text
mtd.writesize = 112KiB
mtd.erasesize = 7 * 1600 * 16KiB = 175MiB
```

112KiB 能整除 175MiB，但两者均不是 2 的幂。

### 6.4 代价

| 项目 | 代价 |
| --- | --- |
| 容量 | parity 固定 12.5%，无独立 commit page |
| 写放大 | 8/7 = 1.143，无额外 commit page program |
| RAM | 至少 112KiB input + 16KiB parity；按 queue depth 倍增 |
| 延迟 | 约一次 8-page multi-plane program；首次验证需读取成员 |
| 失败粒度 | 任一 slot program 失败导致整个 112KiB stripe 无效 |
| 内核代价 | MTD 可直接支持；原生 UBI/UBIFS 改造代价高 |

## 7. Linux 7.0.12 MTD 修改与代价

### 7.1 MTD core 已支持的能力

include/linux/mtd/mtd.h 已提供非 2 次幂的除法/取模回退：

- mtd_div_by_eb() / mtd_mod_by_eb()
- mtd_div_by_ws() / mtd_mod_by_ws()
- mtd_offset_to_wunit() / mtd_wunit_to_offset()

drivers/mtd/mtdcore.c:add_mtd_device() 只在 writesize 或 erasesize 为 2 的幂时设置 shift/mask，否则 shift 为 0并使用上述普通除法路径。

结论：对于 direct MTD driver，无论串行方案上报 16KiB 或 N×page_size，还是并行方案上报 112KiB，MTD core 都不需要结构性修改。非 2 次幂 erasesize 也已经被 MTD helper 支持。

### 7.2 必须避开的通用路径

Linux 7.0.12 仍有路径直接使用 offset & (writesize - 1)：

- drivers/mtd/mtdchar.c 的部分 OOB ioctl。
- drivers/mtd/nand/raw/nand_base.c 的通用 raw NAND read/write。
- NFTL、INFTL 等旧式 FTL 模块。

当前 qemu_3dnand 直接注册 struct mtd_info 回调，不经过 raw NAND framework。最小实现应继续采用 direct MTD：

1. 驱动内部统一使用 div_u64_rem()、div_u64() 或 MTD helper。
2. 不把 112KiB 虚拟 stripe 伪装成 raw NAND 物理 page。
3. 首版不开放通用 OOB 字符设备写接口；驱动通过控制器私有命令在首次 program parity page 时同时写入 main 和 OOB manifest。
4. 不启用 NFTL、INFTL、mtdswap 等未审计消费者。

### 7.3 串行方案的两种 MTD 接口

串行 N+1 是否遇到 2 的幂限制，取决于 MTD 接口如何暴露，而不是取决于物理 stripe 中存在 N+1 个 page。

#### 7.3.1 串行兼容模式：保持物理 page 粒度

```text
mtd.writesize     = physical_page_size = 16KiB
mtd.writebufsize  = 16KiB
mtd.erasesize     = floor(pages_per_block/(N+1)) * N * page_size
```

16KiB 是 2 的幂，因此 UBI 的 min_io_size/max_write_size 检查不会触发。mtd.erasesize 通常不是 2 的幂，但 Linux 7.0.12 的 MTD 和 UBI 均不要求 PEB 大小是 2 的幂。

所以该模式没有需要解除的 2 次幂硬限制，也不需要修改 UBI/UBIFS 的对齐代码。真正的问题是提交语义：

- 前 N-1 个单页 _write() 已向上层返回成功，但 parity 尚未生成。
- 此时掉电会留下没有 parity manifest 的未提交 stripe。
- 如果按本文规则隐藏未提交 stripe，上层已经成功写入的数据会在重启后消失。
- 如果允许读取这些 data page，它们又处于没有 RAID 保护的状态。
- UBI 不保证每个业务写请求都恰好填满驱动定义的 N-page stripe。

因此该模式可以用于功能原型或明确接受“暂时无 RAID 保护”的专用 MTD 用户，但不能在不增加持久化 staging/log 的情况下提供严格的透明 UBI 语义。

#### 7.3.2 串行严格模式：把完整数据组作为最小 I/O

```text
mtd.writesize    = N * physical_page_size
mtd.writebufsize = mtd.writesize
```

如果 physical_page_size 是 2 的幂，则 N×page_size 只有在 N 也是 2 的幂时才是 2 的幂：

| N | 16KiB × N | UBI 2 次幂检查 |
| ---: | ---: | --- |
| 2 | 32KiB | 通过 |
| 3 | 48KiB | 失败 |
| 4 | 64KiB | 通过 |
| 7 | 112KiB | 失败 |
| 8 | 128KiB | 通过 |

严格模式解决了“写成功但 parity 尚未提交”的问题，因为一次 MTD 写调用覆盖完整 N-page 数据组；驱动完成 data、parity 和 manifest 后才返回成功。但当 N 不是 2 的幂时，它会与并行 112KiB 模式一样触发 UBI/UBIFS 的非 2 次幂改造。

### 7.4 并行方案的 MTD 接口

并行方案固定 7D+1P，若向 MTD 暴露完整数据 stripe：

```text
mtd.writesize    = 7 * 16KiB = 112KiB
mtd.writebufsize = 112KiB
mtd.erasesize    = 7 * 1600 * 16KiB = 175MiB
```

112KiB 不是 2 的幂，所以必然触发原生 UBI/UBIFS 限制。并行方案不能简单改成 16KiB 同步写而仍保持“8 页一次并行提交”：驱动必须等待 7 个独立请求，这会产生与串行兼容模式相同的确认/掉电语义问题。

### 7.5 驱动修改点对比

| 修改 | 串行兼容模式 | 串行严格模式 | 并行原生模式 |
| --- | --- | --- | --- |
| mtd.writesize | page_size | N × page_size | 7 × page_size |
| 2 次幂条件 | page_size 为 2 的幂即可 | N 也必须为 2 的幂 | N=7，固定非 2 次幂 |
| mtd.erasesize | block 内 data page 容量 | 同左 | 7 × pages_per_block × page_size |
| _write | 缓存/跟踪 N 个独立 page | 完整组串行写 N+1 | 112KiB 对齐并发起 8-page multi-plane |
| 返回成功点 | 每个 data page 写完 | parity manifest 写完 | 8-page 命令全部成功 |
| 掉电语义 | 高风险 | 清晰 | 清晰，但重启需成员验证 |
| UBI 修改 | 2 次幂方面无需修改 | N 非 2 次幂时需要 | 必须修改 |
| _erase | 擦除对应物理 block | 同左 | 同步擦除 8 个成员 block |
| 坏块 | 物理 block 标坏 | 同左 | 8 个 block 任一坏则屏蔽整组 |

### 7.6 MTD 代价评估

| 路径 | MTD core | 驱动 | 主要风险 |
| --- | ---: | ---: | --- |
| 串行兼容 16KiB | 无 | 中 | 无 2 次幂问题，但同步提交语义不完整 |
| 串行严格且 N 为 2 的幂 | 无 | 中 | UBI 无需解除 2 次幂限制，但写粒度大 |
| 串行严格且 N 非 2 次幂 | 无 | 中 | UBI/UBIFS 需要非 2 次幂改造 |
| 并行原生 112KiB | 无 | 中 | UBI/UBIFS 需要非 2 次幂改造 |
| raw NAND framework + 非 2 次幂虚拟 page | 高 | 高 | 位运算假设分散，不推荐 |

## 8. Linux 7.0.12 UBI 修改与代价

### 8.1 串行与并行方案是否需要修改 UBI

| 方案 | min_io_size | 是否触发 UBI 2 次幂限制 | UBI 修改结论 |
| --- | ---: | --- | --- |
| 串行兼容模式 | 16KiB | 否 | 对齐代码无需修改，但提交语义不满足严格要求 |
| 串行严格，N=2/4/8 等 | 32/64/128KiB 等 | 否 | 对齐代码无需修改 |
| 串行严格，N=3/5/6/7 等 | 48/80/96/112KiB 等 | 是 | 需要本节后续改造 |
| 并行原生 7+1 | 112KiB | 是 | 必须进行本节后续改造 |

因此，“串行方案有无 2 次幂限制”的答案是：串行算法本身没有；保持 16KiB MTD page 时不会触发，严格模式下则由 N 是否为 2 的幂决定。

### 8.2 当前 UBI 硬限制

drivers/mtd/ubi/build.c:io_init() 明确拒绝：

- 非 2 次幂 ubi->min_io_size，其值来自 mtd->writesize。
- 非 2 次幂 ubi->max_write_size，其值来自 mtd->writebufsize。

源码注释说明这不是算法的根本限制，而是为了用位运算避免除法。但不能只删除 is_power_of_2() 校验，因为 UBI 中 ALIGN(x, min_io_size) 和 offset & (min_io_size - 1) 对 112KiB 都会产生错误结果。

### 8.3 非 2 次幂模式必须修改的类别

1. 初始化校验
   - drivers/mtd/ubi/build.c 移除 min_io_size/max_write_size 的 2 次幂检查。
   - 保留 max_write_size >= min_io_size 和整倍数检查。

2. 动态对齐
   - 新增 ubi_align_up(value, unit) 和 ubi_is_aligned(value, unit) 一类 helper。
   - helper 使用余数，不使用只适合 2 次幂的 ALIGN()。
   - 替换 build.c、eba.c、cdev.c、misc.c、upd.c、vtbl.c 中以 min_io_size、max_write_size 或 hdrs_min_io_size 为对齐量的 ALIGN()。

3. 对齐校验
   - drivers/mtd/ubi/kapi.c 中 ubi_leb_write() 和 ubi_leb_change() 的 offset/len & (min_io_size - 1) 改为取模。
   - 全局审计 UBI 对设备几何使用 & (unit - 1) 的路径。

4. subpage 策略
   - 首版固定 mtd->subpage_sft = 0，不支持 N×page_size 虚拟 page 的子页写。
   - hdrs_min_io_size = min_io_size，EC 和 VID header 各占一个最小 I/O 单元。

5. 用户空间工具
   - mtd-utils 中 ubiformat、ubinize、ubiattach 必须做同样的普通整数对齐审计。
   - 镜像记录真实 112KiB min I/O 和 175MiB PEB，不与未修改工具混用。

### 8.4 非 2 次幂严格模式的容量影响

无 subpage 时，每个 PEB 的 EC header 和 VID header 各占一个 min I/O unit。

串行严格方案：

```text
metadata reservation per PEB = 2 * N * page_size
PEB size = floor(pages_per_block/(N+1)) * N * page_size
```

并行 112KiB 方案：

```text
metadata reservation per PEB = 224KiB
PEB size                     = 175MiB
relative overhead            ≈ 0.125%
```

容量百分比很小，但小节点 padding 和运行时缓冲会明显增大。

### 8.5 UBI 代价评估

| 项目 | 预估 |
| --- | --- |
| 改动文件 | 约 8 到 10 个 UBI 文件，外加 mtd-utils |
| 核心改动 | 删除硬检查，替换动态 ALIGN 和位掩码 |
| 运行时开销 | 少量整数除法/取模，相对 NAND I/O 可忽略 |
| 回归范围 | attach、header、volume update、atomic LEB change、recovery |
| 串行兼容/N 为 2 次幂 | 无对齐改造成本；另行承担写粒度或提交语义代价 |
| 串行严格且 N 非 2 次幂 | 中高，与并行方案同类，但具体 min I/O 随 N 变化 |
| 并行原生 112KiB | 中高，且为固定必选改造 |

## 9. UBIFS 影响：不能只修改 UBI

本节只适用于“串行严格且 N 非 2 次幂”和“并行原生 112KiB”两类模式。串行兼容 16KiB 或串行严格且 N 为 2 的幂时，不需要解除 UBIFS 的 2 次幂限制。

如果非 2 次幂模式只使用 UBI raw/static volume，完成第 8 节可停在 UBI 层。如果还要挂载 UBIFS，Linux 7.0.12 的 fs/ubifs 也必须修改。

fs/ubifs/super.c 当前会：

- 拒绝非 2 次幂 min_io_size/max_write_size。
- 用 fls(size)-1 填充 min_io_shift/max_write_shift。
- 在大量位置用 ALIGN(value, c->min_io_size) 或 ALIGN(value, c->max_write_size)。

所需修改：

- 引入普通整数 align-up/down/is-aligned helper。
- 让 min_io_shift/max_write_shift 仅在 2 次幂时有效，或去除相关依赖。
- 替换 io.c、recovery.c、super.c、sb.c、log.c、lpt*.c、tnc_commit.c、orphan.c、scan.c 等处的动态 ALIGN。
- 重点回归 write buffer boundary、replay、recovery、master node、log head 和 LPT commit。

代价预估：

| 项目 | 预估 |
| --- | --- |
| 改动文件 | 约 12 到 18 个 UBIFS 文件 |
| 代码难度 | 中；恢复和边界条件难以穷举 |
| 测试成本 | 高，需要掉电点注入和长期压力测试 |
| 总风险 | 高 |

## 10. 四条落地路径

### 10.1 路径 A：专用 direct MTD 原型，推荐首选

- 不修改 MTD core、UBI 和 UBIFS。
- 只允许专用测试程序提交完整 stripe。
- 串行 profile 使用 N × page_size 写粒度。
- 并行 profile 使用 112KiB 写粒度。
- 先验证布局、XOR、单页恢复、parity manifest、未提交检测和 multi-plane 性能。

代价低，但不能直接挂载 UBI/UBIFS。

### 10.2 路径 B：串行保持 16KiB MTD 兼容面

- mtd.writesize = mtd.writebufsize = physical_page_size。
- 串行驱动等待 N 个独立 page 后生成 parity。
- UBI/UBIFS 不修改。

表面代价中等，但单页 _write() 返回成功时 stripe 可能尚未提交。要严格解决只能增加持久化 staging log，这会重新引入类似日志/FTL 的复杂度。因此不建议作为产品方案。

并行方案不采用本路径，因为等待 7 个单页请求会削弱一次 7D+1P 提交的接口语义和性能确定性。

### 10.3 路径 C：串行选择 2 次幂 N

- 采用串行严格模式。
- 选择 N=2、4、8 等，使 N×page_size 仍是 2 的幂。
- MTD 一次请求覆盖完整数据组，parity manifest 成功后才返回。
- 不需要修改 UBI/UBIFS 的 2 次幂对齐逻辑。

这是需要严格提交语义且必须兼容原生 UBI/UBIFS 时，串行方案代价最低的选择。代价是 RAID 比例由内核兼容性反向约束，不能任意选择 N；它也不适用于固定 7D+1P 的并行几何。

### 10.4 路径 D：原生非 2 次幂 UBI/UBIFS

- 串行严格方案使用非 2 次幂 N，或并行方案暴露 112KiB writesize。
- 按第 8 节改造 UBI 和 mtd-utils。
- 需要文件系统时按第 9 节改造 UBIFS。

代价高，但上层 I/O 原子边界与 page-RAID stripe 一致，语义最干净，也最能利用 multi-plane 并行。

### 10.5 推荐结论

从“不实现 FTL/GC、最小代价实现 page-RAID”的目标出发：

1. 先做路径 A，完成串行和并行功能及可靠性验证。
2. 串行方案若必须兼容原生 UBI/UBIFS，优先评估路径 C，选择 2 次幂 N。
3. 并行 7+1 若必须进入 UBI，或串行必须使用非 2 次幂 N，再采用路径 D 改造 UBI/mtd-utils。
4. UBIFS 成为硬需求后，才扩展到 UBIFS 对齐和恢复路径。
5. 不推荐路径 B 产品化；它没有 2 次幂问题，但把持久化问题隐藏在驱动缓存中，最终容易演变成更复杂的日志系统。

## 11. 异常处理

| 场景 | 处理 |
| --- | --- |
| 串行 data program fail | 不写 parity，整组 incomplete |
| 串行 parity program fail | parity ECC/CRC 无效，整组 incomplete |
| 并行 data slot 失败但 parity 成功 | manifest 为候选；成员 CRC 验证失败，整组 corrupt |
| 并行 parity slot 失败 | 无有效 manifest，整组 incomplete |
| 单 data page ECC 不可纠 | 尝试 XOR 恢复 |
| parity page 失效 | data 仍可读，但失去 RAID 恢复能力 |
| 两个及以上成员失效 | 返回 -EBADMSG |
| erase 部分失败 | 整个逻辑 eraseblock 标坏 |
| 并行提交部分失败 | 整个 7D+1P stripe invalid |
| 序号或角色不一致 | 视为元数据损坏，不猜测恢复 |

## 12. 测试计划

### 12.1 映射测试

- 使用非 2 次幂 page size、N 和 pages-per-block 测试除法/取模边界。
- 覆盖 stripe 首尾、block 尾部 unused page 和最后一个 block。
- 验证物理地址不重叠、不越界。

### 12.2 功能和故障注入

- 串行 N+1 完整 stripe 读写和 parity 比对。
- 并行 7+1 的 112KiB 读写和 multi-plane 命令计数。
- 分别对每个 data slot 注入单页错误并恢复。
- 测试 parity 失效、双页失效和 CRC 错误。
- 串行方案在任意 data 和 parity 写入前后注入掉电。
- 并行方案在 8-page multi-plane program 的不同成员完成点注入掉电，覆盖 parity 成功但 data 失败的情况。

成功标准：串行方案只有有效 parity manifest 的组可见；并行方案只有 manifest 和全部成员 CRC 均有效的组可见；未完成组不被当作有效数据；不需要 GC 或自动补写。

### 12.3 UBI/UBIFS 回归，仅路径 C

- 以 112KiB min I/O、175MiB PEB 执行 ubiformat/ubiattach。
- 测试 dynamic/static volume、update、rename、remove 和 atomic LEB change。
- 测试 UBIFS mount、fsync、remount、replay 和 recovery。
- 对关键写路径做掉电点注入。
- 使用常见 2 次幂 NAND 几何跑全量回归，确保快速路径无退化。

## 13. 实现前必须确认的硬件条件

- “双 plane、每 plane 4 page”的确切并行命令和地址约束。
- 8 个 page 是否可在一个 controller transaction 中并行提交。
- multi-plane program 对 block/page address 相同性的要求。
- OOB 是否参与 ECC、可用字节数，以及控制器能否在首次 page program 时同时提供 main 和 OOB。
- program fail 能否定位到单个 slot。
- DMA descriptor 数量、对齐、scatter-gather 和最大传输长度。
