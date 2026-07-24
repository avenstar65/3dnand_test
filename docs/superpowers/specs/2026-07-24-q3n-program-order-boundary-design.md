# Q3N 编程顺序职责边界修订设计

## 1. 修订目的

本设计修正 Q3N v2 LDPC/Page-RAID 方案中对物理页编程顺序的职责划分。

块内顺序写是上层文件系统的使用约束。本阶段 QEMU 和
`qemu_3dnand` 驱动都不维护、恢复或强制检查下一个可编程页。本文覆盖旧设计中
关于 `next_prog_page`、program frontier、页序错误、顺序等待、逐页状态和
UNPROTECTED tombstone 的要求。LDPC、BBM、MTD ECC 和 RAID 恢复的其他语义
保持不变。

本阶段不设计驱动卸载、虚拟机重启或宿主机重启后的 RAID 运行时状态恢复。

## 2. QEMU 的介质职责

QEMU 只模拟本方案需要的 NAND 数据行为：

- PROGRAM 对 main 和 physical OOB 的每个字节执行
  `stored_byte = old_byte & incoming_byte`；
- PROGRAM 可以访问任意合法物理页，不检查页号递增关系；
- 同一页可以被重复 PROGRAM，结果仍只允许 bit 从 1 变为 0；
- ERASE 把整个物理块的 main、physical OOB 和 bitflip overlay 恢复为擦除态；
- 第一物理页的 physical OOB[0] 仍是 BBM；
- controller 继续负责生成/验证模拟 LDPC、logical/physical OOB 映射以及
  bitflip/ECC 结果；
- 坏块、越界访问、底层 I/O 和显式故障注入仍可使命令失败。

QEMU 不再：

- 保存或暴露 `next_prog_page`；
- 保存 `ERASED/PRESENT/LOST` 逐页状态；
- 拒绝跳页、倒序或重复 PROGRAM；
- 统计页序错误；
- 根据 program frontier 判断页面是否擦除。

controller 需要判断 LDPC 擦除态时，直接检查读取到的 main 和 physical OOB。
main 与完整 physical OOB 都为 `0xff` 才是擦除码字。正常 PROGRAM 即使 main
全为 `0xff`，controller 也会生成确定性 LDPC，因此不依赖额外逐页状态来区分。

重复 PROGRAM 后，LDPC 区和 main 一样执行按位与。如果最终 LDPC 不再匹配最终
main，后续读取按既有规则报告不可纠，而不是由 QEMU 预先拒绝该 PROGRAM。

旧的持久化 `LOST` 页状态一并删除。需要模拟不可纠页时使用超过 LDPC strength
的持久化 bitflip overlay；需要模拟操作失败时使用现有的 read/program 故障注入。

## 3. 驱动职责

驱动不维护 `next_prog_page`，也不根据物理页前沿决定请求是否可执行。

需要删除的驱动行为包括：

- program frontier 初始化、推进和擦除重置；
- 前沿依赖等待；
- 落后请求的 `-ESTALE` 或其他页序错误；
- 跨越前沿请求的等待或拒绝；
- 从 QEMU block status 读取下一编程页；
- 驱动侧或 QEMU 侧的 `order_errors` 统计。

逻辑页到物理页的固定映射仍然是每个 stripe 七个数据页加一个隐藏 parity 页：

```text
logical pages:  L0 L1 L2 L3 L4 L5 L6 L7 ...
physical pages: D0 D1 D2 D3 D4 D5 D6 P  D0 ...
```

文件系统顺序写入七个逻辑数据页后，下一个逻辑页直接映射到下一 stripe 的 D0。
它不依赖前一 stripe 的 P 是否已写入。后台 P 可以在下一 stripe 的前台数据写入
之前或之后完成，QEMU 都接受对应的物理 PROGRAM。

## 4. RAID 运行时语义

驱动为每个运行时 stripe 维护 D0..D6 的成功位图，但该位图只表示 RAID
构建条件，不表示 NAND program frontier：

1. 数据页 PROGRAM 成功后设置对应 lane；
2. 七个 lane 都成功且 metadata 均可验证时，排队生成 parity；
3. parity worker 通过 LDPC 读取 D0..D6，任一来源不可纠则不发布 parity；
4. parity PROGRAM 和 manifest 都成功后，stripe 才进入 PROTECTED；
5. parity PROGRAM 失败时，stripe 保持 UNPROTECTED；
6. 无论 parity 成功、失败或仍在排队，下一 stripe 的数据 I/O 都可以继续。

不再写 UNPROTECTED tombstone。无法保护的 stripe 只保留驱动内存状态，P 页不做
占位 PROGRAM。由于本阶段不做重启恢复，不需要在介质中持久化“该 stripe 已确定
无保护”的占位记录。

目标页 LDPC 不可纠时，只能使用当前运行期内已经发布且 manifest/CRC 验证通过的
PROTECTED stripe 做 RAID 恢复。没有有效运行时保护状态时返回 `-EBADMSG`。

## 5. 命令和错误语义

QEMU PROGRAM 不再产生任何页序相关错误。命令失败仅来自：

- 物理地址或长度非法；
- BBM 表示坏块；
- 后端介质 I/O 失败；
- 显式 read/program 故障注入。

重复 PROGRAM 的命令可以成功，但读取结果可能因为 main/OOB/LDPC 的按位与结果
不一致而形成 ECC failure。这是介质数据结果，不是 program-order error。

驱动不再返回页序相关的 `-ESTALE`、`-ERANGE` 或人为 `-EBUSY`。MTD 的
bitflip、`-EUCLEAN`、`-EBADMSG` 和最终 `ecc_stats` 规则不变。

## 6. ABI 和介质格式调整

从 QEMU/Linux 共享 ABI 中删除或停止使用：

- `Q3N_REG_BLOCK_NEXT_PAGE`；
- `Q3N_REG_STAT_ORDER_ERRORS`；
- `GET_BLOCK_STATUS` 返回的 next-page 语义。

`GET_BLOCK_STATUS` 若继续保留，只报告从第一页 BBM 得出的坏块状态，不报告
program frontier 或逐页状态。

Q3NMEDIA v2 不再保存 block `next_prog_page` 数组和逐页 state 数组。介质只需
持久化 header、main+physical OOB 以及 bitflip overlay。由于当前 v2 尚处开发
阶段，本次直接修订 v2 布局，不再引入额外版本号；旧的开发期 v2 镜像可以明确
拒绝并重新创建。

## 7. 验证要求

QEMU 单元/集成测试至少覆盖：

1. 擦除后任意物理页都可首先 PROGRAM；
2. page 8 可以在 page 7 之前 PROGRAM；
3. 同一页重复 PROGRAM 成功，最终内容等于逐字节按位与；
4. PROGRAM 不产生 order error，ABI 不再暴露 program frontier；
5. ERASE 恢复 main、physical OOB 和 overlay；
6. 全 `0xff` 擦除页与 main 为 `0xff`、但具有有效 LDPC 的已编程页可区分；
7. 重复 PROGRAM 导致 LDPC 不匹配时返回既定 ECC uncorrectable 结果；
8. BBM、logical OOB 映射和 bitflip 注入行为不回退。

驱动 KUnit/guest 测试至少覆盖：

1. 调度器不存在 program-frontier 等待和页序拒绝分支；
2. 下一 stripe D0 不受上一 stripe P 未完成或失败影响；
3. D0..D6 全部成功后只生成一次 parity；
4. 数据页失败、metadata 无效、parity 来源不可纠或 parity PROGRAM 失败时，
   stripe 保持 UNPROTECTED 且不写 tombstone；
5. 后续 stripe 仍可继续写入并独立形成 PROTECTED；
6. LDPC 39/40/41 bit、MTD corrected/failed、RAID 恢复及 BBM 测试继续通过；
7. QEMU、kernel、rootfs 构建和 guest smoke test 全部通过。

## 8. 非目标

- 不保证或检查文件系统是否真的按物理块内顺序写入；
- 不恢复驱动重载或虚拟机重启前的 stripe 位图和 PROTECTED 状态；
- 不设计持久化 program journal；
- 不实现真实 LDPC 编码器或迭代译码器；
- 不修改通用 MTD、UBI 或 UBIFS。
