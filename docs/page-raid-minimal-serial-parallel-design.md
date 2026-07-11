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
        | commit scanner        |
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
| commit scanner | 启动时识别 committed、incomplete、corrupt stripe |
| recovery | ECC 不可纠时读取其余成员并恢复 |

## 4. 最小持久化格式

每个物理 page 的 OOB 保存：

| 字段 | 建议宽度 | 说明 |
| --- | ---: | --- |
| magic | 16 bit | page-RAID 标识 |
| format_version | 8 bit | 布局版本 |
| profile | 8 bit | serial-N+1 或 parallel-7+1 |
| group_seq | 64 bit | stripe 序号 |
| role | 8 bit | data slot、parity 或 commit |
| member_bitmap | 16 bit | 预期成员集合 |
| payload_crc | 32 bit | 可选的静默损坏检测 |
| header_crc | 32 bit | 元数据校验 |

默认不依赖同一 NAND page 的二次编程，而使用 append-only commit record：

```text
commit = { magic, version, profile, group_seq,
           member_bitmap, parity_location, header_crc }
```

一个 commit page 可容纳多条定长记录。扫描规则：

1. 成员完整、元数据一致且存在有效 commit：COMMITTED。
2. 只有部分成员或没有 commit：INCOMPLETE，不向上层暴露。
3. 有 commit 但成员不足或 CRC 不一致：CORRUPT，返回 -EBADMSG。
4. 全 0xff 且没有 commit：EMPTY。

只有确认硬件支持安全的 OOB 部分页二次编程时，才考虑用 OOB commit bit 替代独立记录。

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
4. data 和 parity 均成功后追加 commit record。
5. commit 落盘后整组才有效。

推荐严格模式：一次请求必须包含 N × page_size 数据，驱动不保留跨请求脏缓存。

兼容模式可让 mtd.writesize 保持物理 page_size，并缓存 N 个独立写请求，但前 N-1 次写返回成功时 stripe 尚未提交。除非再增加持久化 staging area，否则这不满足严格同步写语义，因此不建议用于 UBI。

### 5.3 读恢复

正常读取只访问目标 data page。ECC 不可纠时：

1. 确认 stripe 已提交。
2. 读取 parity 和其余 N-1 个 data page。
3. 只有一个成员失败时执行 XOR 恢复。
4. 校验 payload CRC；成功则返回数据，失败返回 -EBADMSG。

### 5.4 代价

| 项目 | 代价 |
| --- | --- |
| 容量 | parity 基本开销为 1/(N+1)，commit 通常小于 1% |
| 写放大 | 不含 commit 时为 (N+1)/N |
| RAM | 严格流式 XOR 约 page_size；整组缓存为 N × page_size |
| 延迟 | N 个 data program + parity + commit，基本串行 |
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
5. 全部成功后追加 commit record。
6. 任一 slot 失败则不提交，整组无效。

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
| 容量 | parity 固定 12.5%，commit 通常小于 1% |
| 写放大 | 不含 commit 时为 8/7 = 1.143 |
| RAM | 至少 112KiB input + 16KiB parity；按 queue depth 倍增 |
| 延迟 | 约一次 multi-plane program + commit |
| 失败粒度 | 任一 slot program 失败导致整个 112KiB stripe 无效 |
| 内核代价 | MTD 可直接支持；原生 UBI/UBIFS 改造代价高 |

## 7. Linux 7.0.12 MTD 修改与代价

### 7.1 MTD core 已支持的能力

include/linux/mtd/mtd.h 已提供非 2 次幂的除法/取模回退：

- mtd_div_by_eb() / mtd_mod_by_eb()
- mtd_div_by_ws() / mtd_mod_by_ws()
- mtd_offset_to_wunit() / mtd_wunit_to_offset()

drivers/mtd/mtdcore.c:add_mtd_device() 只在 writesize 或 erasesize 为 2 的幂时设置 shift/mask，否则 shift 为 0并使用上述普通除法路径。

结论：对于 direct MTD driver，MTD core 不需要为 112KiB writesize 或 175MiB erasesize 做结构性修改。

### 7.2 必须避开的通用路径

Linux 7.0.12 仍有路径直接使用 offset & (writesize - 1)：

- drivers/mtd/mtdchar.c 的部分 OOB ioctl。
- drivers/mtd/nand/raw/nand_base.c 的通用 raw NAND read/write。
- NFTL、INFTL 等旧式 FTL 模块。

当前 qemu_3dnand 直接注册 struct mtd_info 回调，不经过 raw NAND framework。最小实现应继续采用 direct MTD：

1. 驱动内部统一使用 div_u64_rem()、div_u64() 或 MTD helper。
2. 不把 112KiB 虚拟 stripe 伪装成 raw NAND 物理 page。
3. 首版不开放通用 OOB 字符设备写接口；私有元数据通过驱动的物理命令访问。
4. 不启用 NFTL、INFTL、mtdswap 等未审计消费者。

### 7.3 驱动修改点

| 修改 | 串行严格模式 | 并行原生模式 |
| --- | --- | --- |
| mtd.writesize | N × page_size | 7 × page_size |
| mtd.writebufsize | 等于 writesize | 等于 writesize |
| mtd.erasesize | floor(pages_per_block/(N+1)) × N × page_size | 7 × pages_per_block × page_size |
| mtd.size | 可用逻辑 eraseblock 数 × erasesize | 可用 block-group 数 × erasesize |
| _write | 完整 stripe 校验并串行写 N+1 | 112KiB 对齐并发起 8-page multi-plane |
| _read | 逻辑 stripe 拆成 N 个 page | 逻辑 stripe 拆成 7 个 data page |
| _erase | 擦除相应物理 block/元数据区 | 同步擦除 8 个成员 block |
| 坏块 | 任一成员坏则屏蔽逻辑 eraseblock | 8 个 block 任一坏则屏蔽整组 |

### 7.4 MTD 代价评估

| 路径 | MTD core | 驱动 | 风险 |
| --- | ---: | ---: | --- |
| 16KiB 可见 page + 驱动聚合 | 无 | 中 | 同步写语义风险高 |
| direct MTD + N×page 或 112KiB writesize | 无 | 中 | MTD 可行，UBI/UBIFS 需改造 |
| raw NAND framework + 非 2 次幂虚拟 page | 高 | 高 | 位运算假设分散，不推荐 |

## 8. Linux 7.0.12 UBI 修改与代价

### 8.1 当前硬限制

drivers/mtd/ubi/build.c:io_init() 明确拒绝：

- 非 2 次幂 ubi->min_io_size，其值来自 mtd->writesize。
- 非 2 次幂 ubi->max_write_size，其值来自 mtd->writebufsize。

源码注释说明这不是算法的根本限制，而是为了用位运算避免除法。但不能只删除 is_power_of_2() 校验，因为 UBI 中 ALIGN(x, min_io_size) 和 offset & (min_io_size - 1) 对 112KiB 都会产生错误结果。

### 8.2 必须修改的类别

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
   - 首版固定 mtd->subpage_sft = 0，不支持 112KiB 虚拟 page 的子页写。
   - hdrs_min_io_size = min_io_size，EC 和 VID header 各占一个最小 I/O 单元。

5. 用户空间工具
   - mtd-utils 中 ubiformat、ubinize、ubiattach 必须做同样的普通整数对齐审计。
   - 镜像记录真实 112KiB min I/O 和 175MiB PEB，不与未修改工具混用。

### 8.3 容量影响

112KiB min I/O 且无 subpage 时，每个 PEB 的 EC header 和 VID header 各占一个 I/O unit：

```text
metadata reservation per PEB = 224KiB
PEB size                     = 175MiB
relative overhead            ≈ 0.125%
```

容量百分比很小，但小节点 padding 和运行时缓冲会明显增大。

### 8.4 UBI 代价评估

| 项目 | 预估 |
| --- | --- |
| 改动文件 | 约 8 到 10 个 UBI 文件，外加 mtd-utils |
| 核心改动 | 删除硬检查，替换动态 ALIGN 和位掩码 |
| 运行时开销 | 少量整数除法/取模，相对 NAND I/O 可忽略 |
| 回归范围 | attach、header、volume update、atomic LEB change、recovery |
| 总风险 | 中高 |

## 9. UBIFS 影响：不能只修改 UBI

如果只使用 UBI raw/static volume，完成第 8 节可停在 UBI 层。如果还要挂载 UBIFS，Linux 7.0.12 的 fs/ubifs 也必须修改。

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

## 10. 三条落地路径

### 10.1 路径 A：专用 direct MTD 原型，推荐首选

- 不修改 MTD core、UBI 和 UBIFS。
- 只允许专用测试程序提交完整 stripe。
- 串行 profile 使用 N × page_size 写粒度。
- 并行 profile 使用 112KiB 写粒度。
- 先验证布局、XOR、单页恢复、未提交检测和 multi-plane 性能。

代价低，但不能直接挂载 UBI/UBIFS。

### 10.2 路径 B：保持 16KiB MTD 兼容面

- mtd.writesize = mtd.writebufsize = physical_page_size。
- 驱动等待 N 个或 7 个独立 page 后生成 parity。
- UBI/UBIFS 不修改。

表面代价中等，但单页 _write() 返回成功时 stripe 可能尚未提交。要严格解决只能增加持久化 staging log，这会重新引入类似日志/FTL 的复杂度。因此不建议作为产品方案。

### 10.3 路径 C：原生非 2 次幂 UBI/UBIFS

- MTD 暴露 N × page_size 或 112KiB writesize。
- 按第 8 节改造 UBI 和 mtd-utils。
- 需要文件系统时按第 9 节改造 UBIFS。

代价高，但上层 I/O 原子边界与 page-RAID stripe 一致，语义最干净，也最能利用 multi-plane 并行。

### 10.4 推荐结论

从“不实现 FTL/GC、最小代价实现 page-RAID”的目标出发：

1. 先做路径 A，完成串行和并行功能及可靠性验证。
2. UBI raw volume 成为硬需求后，再改造 UBI 和 mtd-utils。
3. UBIFS 成为硬需求后，才扩展到 UBIFS 对齐和恢复路径。
4. 不推荐路径 B 产品化；它将持久化问题隐藏在驱动缓存中，最终容易演变成更复杂的日志系统。

## 11. 异常处理

| 场景 | 处理 |
| --- | --- |
| data program fail | 不写 commit，整组 invalid |
| parity program fail | 不写 commit，整组 invalid |
| commit program fail | 整组 incomplete，重启扫描时忽略 |
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
- 在任意 data、parity、commit 写入前后注入掉电。

成功标准：已提交组始终可识别；未提交组始终不被当作有效数据；不需要 GC 或自动补写。

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
- OOB 是否参与 ECC、可用字节数和部分页编程限制。
- program fail 能否定位到单个 slot。
- DMA descriptor 数量、对齐、scatter-gather 和最大传输长度。
