# QEMU 3D NAND 物理介质持久化与 OOB 坏块标记设计

## 1. 目标

为现有 `q3n-nand` QEMU 控制器增加跨 QEMU 进程正常退出和重新启动的
物理 NAND 介质持久化，并让 Linux `qemu_3dnand` 驱动实现可持久恢复的
`_block_markbad()`。

持久化范围包括：

- 物理 page 的 16 KiB main data；
- 物理 page 的 1 KiB OOB；
- page 的 erased、programmed/present、programmed/lost 状态；
- 每个物理 block 的严格升序编程游标；
- block 第一个 page 的 `OOB[0]` 坏块标记。

本阶段只保证 QEMU 正常关机后的恢复。允许丢失正在异常终止时尚未 flush 的
最后状态；不保证 `kill -9` 或主机在任意字节写入中断时的事务一致性。

## 2. 分层边界

### 2.1 QEMU 只负责物理 NAND

QEMU 只理解物理 block、物理 page、main、OOB、program、erase、坏块标记和
严格升序编程约束。QEMU 不理解：

- `D0..D6,P`；
- stripe、parity index 或 generation；
- MTD 逻辑 eraseblock；
- UBI/UBIFS；
- data/parity/meta/reserve pool 的运行时布局策略；
- 坏块替换、FTL 或 GC。

镜像 header 只描述固定的物理几何和镜像格式。现有 pool 参数仍可作为设备
几何信息暴露给驱动，但不写入任何 Page-RAID 语义。

### 2.2 Linux 驱动负责布局和恢复

Linux `qemu_3dnand` 驱动继续独占管理：

- MTD 逻辑地址到物理 block/page 的映射；
- 同一物理 block 内的串行 `D0..D6,P` 布局；
- P0/P1/P2 调度优先级；
- parity 生成、恢复和 generation；
- markbad 前的 parity cancel barrier；
- 物理状态到 MTD `_block_isbad()` 的映射。

不修改 MTD、UBI 或 UBIFS core。

## 3. 选型

采用 QEMU `BlockBackend` 加版本化固定布局稀疏镜像。

未采用设备直接 `open/pread/pwrite`，因为它绕过 QEMU 的存储权限、错误传播和
flush 机制。未采用 append-only 日志，因为文件会持续增长并最终需要
checkpoint/GC，与当前最小复杂度目标冲突。

固定布局镜像的逻辑大小约为 55 GiB，但未写 page 保持 sparse hole，实际磁盘
占用只随已编程 page 增长。

## 4. 镜像格式 v1

所有多字节字段使用 little-endian。镜像由以下区域组成：

```text
0
+-----------------------------+
| 4 KiB versioned header      |
+-----------------------------+
| block state array           |  next_prog_page per physical block
+-----------------------------+
| page state array            |  one byte per physical page
+-----------------------------+
| padding to 4 KiB            |
+-----------------------------+
| fixed physical page slots   |  16 KiB main + 1 KiB OOB per page
+-----------------------------+
```

### 4.1 Header

Header 至少包含：

- 8-byte magic：`Q3NMEDIA`；
- format version：`1`；
- header size：`4096`；
- page size：`16384`；
- OOB size：`1024`；
- pages per block：`1600`；
- physical block count；
- block-state offset/length；
- page-state offset/length；
- page-slot offset/stride；
- expected image size。

打开已有镜像时，magic、版本、几何、区域边界或文件长度不匹配均使设备
realize 失败。设备不得静默格式化或截断不兼容镜像。

### 4.2 Block state

每个物理 block 持久化 `next_prog_page`。该字段仅表示物理编程前沿，不包含
stripe 或 parity 语义。

坏块状态不作为独立字段保存。内存中的 `block_meta.bad` 只能是从物理
`OOB[0]` 重建的缓存。

### 4.3 Page state

每个物理 page 使用一个字节：

- `0`：erased，读取 main/OOB 默认返回 `0xff`；
- `1`：programmed/present，page slot 包含有效 main/OOB；
- `2`：programmed/lost，物理编程前沿仍保留，但读取返回 `-EIO`。

`programmed/lost` 用于保持现有 data-loss fault injection 跨正常重启后的语义。
严格升序约束保证 `next_prog_page` 之前没有 erased hole；lost page 仍占用物理
编程位置。

### 4.4 Page slot 与 sparse 行为

每个物理 page 固定占用 `16384 + 1024` 字节。物理页号直接决定 slot offset，
不使用哈希表或 Page-RAID 映射。现有进程内 `pages` 哈希表由 BlockBackend
固定偏移读写替代，避免同一份介质同时存在两个事实源。

新镜像只写 header、状态区，并显式把每个物理 block 首个 page slot 的
`OOB[0]` 初始化为 `0xff`。其余 erased page 的 sparse hole 不被读取；控制器
根据 page state 直接返回全 `0xff`。

## 5. 坏块标记模型

本设备为大页 NAND，遵循当前 Linux 7.0.12 默认位置：

```text
physical block first page, OOB offset 0
```

语义为：

- `OOB[0] == 0xff`：good；
- `OOB[0] != 0xff`：bad；
- 驱动 markbad 时写入 `0x00`。

`MARK_BAD_BLOCK` 是专用控制器命令，不是通用 OOB-only 写接口。该命令只把
首个 page slot 的 `OOB[0]` 从 `0xff` 编程为 `0x00`：

- 不修改 main data；
- 不改变 page state；
- 不推进 `next_prog_page`；
- 重复标记幂等成功；
- 写入失败时命令返回 error，内存 bad cache 不得先行更新。

组合 main+OOB 读取 block 首个 page 时，即使 page state 为 erased，也必须把
真实 `OOB[0]` 覆盖到返回的 OOB buffer。bad block 的普通 erase/program 被
拒绝，因此 marker 不会被擦除或覆盖。底层 read 仍允许执行。

## 6. QEMU BlockBackend 生命周期

`q3n-nand-pci` 暴露 `drive` 属性，并把 `BlockBackend` 传给内部
`q3n-nand` 设备。设备没有 backend 时 realize 失败，避免无意退化成易失介质。

### 6.1 新镜像

脚本先创建长度为 0 的普通文件。设备发现 backend 长度为 0 后：

1. 计算 v1 expected image size；
2. 通过 BlockBackend 扩展逻辑长度；
3. 写 header 和初始状态区；
4. 初始化所有首 page `OOB[0]` 为 `0xff`；
5. flush backend；
6. 完成 realize。

### 6.2 已有镜像

设备读取并校验 header，然后加载 block/page state。每个物理 block 的 bad
cache 通过读取首 page slot 的 `OOB[0]` 建立。任一读错误或格式不匹配均使
realize 失败。

### 6.3 命令持久化与关闭

program、erase、markbad 和 fault injection 在内存状态更新成功前先完成对应
BlockBackend 写入。单个命令失败时返回 controller error，并保留可判断的旧
内存状态。

正常 QEMU 关闭时，设备 unrealize 调用 backend flush。普通 controller reset
只清命令/IRQ/PIO 临时状态，不清物理介质、编程游标或 BBM。

## 7. 控制器 ABI

在 QEMU 和 Linux 镜像头文件中保持完全一致：

- capability：`Q3N_CAP_PERSISTENT_MEDIA`；
- capability：`Q3N_CAP_BAD_BLOCK_MARKER`；
- command：`Q3N_CMD_GET_BLOCK_STATUS`；
- command：`Q3N_CMD_MARK_BAD_BLOCK`；
- register：`Q3N_REG_BLOCK_STATUS`；
- register：`Q3N_REG_BLOCK_NEXT_PAGE`。

Linux 先通过 `ADDR_LO/HI` 设置物理 block 首 page 地址，再执行命令。

`GET_BLOCK_STATUS`：

- 校验地址严格对齐物理 block；
- 读取首 page `OOB[0]`；
- 返回 BAD/ERASED 等物理状态位；
- 返回 `next_prog_page`；
- 不返回任何 stripe/parity 信息。

`MARK_BAD_BLOCK`：

- 校验地址严格对齐物理 block；
- 专用编程首 page `OOB[0] = 0x00`；
- 重复执行成功；
- backend 写失败时设置 controller error。

现有 `Q3N_CAP_BASIC_FLASH` 保留。驱动缺少新 capability 时继续让
`_block_markbad()` 返回 `-EOPNOTSUPP`，保持旧 QEMU 兼容性。

## 8. Linux probe 恢复

驱动在分配 data metadata、program state 和 parity index 后、注册 MTD 前，
逐个查询可见物理 data block。

对每个 block：

1. 读取 BAD 和 `next_prog_page`；
2. 校验 `next_prog_page <= pages_per_block`；
3. 从 BBM 设置 `data_meta[block].bad`；
4. 设置 `program_state[block].next_prog_page`；
5. 设置 `data_meta[block].erased = (next_prog_page == 0)`；
6. 为编程前沿内 `page % 8 < 7` 的 page 重建 data-valid；
7. 为编程前沿内 `page % 8 == 7` 的 page 重建 parity index。

这里的 `% 8` 解释只存在于 Linux 驱动。QEMU 只返回物理编程前沿。

重启时没有旧 worker，驱动为恢复出的 block 创建新的内存 generation，并让
恢复出的 parity entry 引用该 generation。parity entry 的物理位置由同 block
串行布局唯一确定。

### 8.1 尾部 stripe

- `next_prog_page % 8` 为 `0..6`：允许存在，表示空 stripe 或只完成部分 data；
- 余数为 `7`：表示 D0..D6 已完成但 parity 未完成。

对于余数 7，驱动必须在 MTD 注册前读取七个 data page、重建并编程 P，成功后
把物理前沿推进到下一个 stripe。恢复失败则 probe 失败，不能注册一个无法
继续严格升序写的 MTD。

## 9. Linux markbad 路径

`_block_markbad()` 使用现有 per-block cancel ownership：

1. 校验 MTD 地址并计算物理 block；
2. 获取 `mtd_lock`；
3. 已缓存为 bad 时幂等返回 0；
4. 获取 block cancel ownership；
5. 释放 `mtd_lock`，等待该 block pending parity 归零；
6. 重新获取 `mtd_lock` 并执行物理 `MARK_BAD_BLOCK`；
7. 仅在控制器成功后设置 `data_meta[block].bad = true`；
8. 结束 cancel ownership 并唤醒必要等待者。

任何失败路径都必须由 owner 执行一次 `cancel_end()`。未获得 ownership 的并发
markbad/erase 返回 `-EBUSY`，不得清除其他调用者的 cancelling 状态。

驱动 write/erase 在进入物理命令前检查 bad cache 并返回 `-EIO`。read 保留
NAND 常见语义，允许底层读取；MTD/UBI 通过 `_block_isbad()` 避免正常使用。

## 10. 启动脚本

`scripts/run-qemu.sh` 新增：

- 默认镜像：`work/media/q3n-nand.raw`；
- `--fresh-nand`：删除默认或指定镜像，创建长度为 0 的新文件；
- `--nand-image PATH`：使用指定镜像；
- 无选项时复用已有镜像。

脚本使用 `-drive if=none,id=q3n-media,format=raw,file=...` 和
`-device q3n-nand-pci,drive=q3n-media`。路径必须位于当前容器可访问的共享目录。

`--fresh-nand` 是唯一允许的自动清空入口。设备遇到损坏或不兼容镜像时不自动
删除数据。

## 11. 验收

### 11.1 QEMU 层

- 新镜像格式化并成功重新打开；
- main/OOB、page state 和 `next_prog_page` 重开后恢复；
- erase 后旧 slot 不可见；
- data-loss 的 programmed/lost 状态重开后仍返回 `-EIO`；
- markbad 实际把首 page slot 的 `OOB[0]` 从 `0xff` 写成 `0x00`；
- 重开后 BBM 仍为 `0x00`；
- bad block program/erase 被拒绝，read 可执行；
- 无效 magic、版本、几何或长度使 realize 失败。

### 11.2 Linux/KUnit

- 双方 ABI 常量完全一致；
- 物理 status 到 program frontier/data-valid/parity index 的重建测试；
- 尾部余数 7 的 parity 完成测试；
- markbad cancel ownership、幂等和控制器失败路径测试；
- bad block write/erase 拒绝测试；
- 既有 KUnit 全部通过。

### 11.3 两次 QEMU 启动的 guest smoke

第一次使用 `--fresh-nand`：

1. 写入完整 stripe 和可识别数据；
2. 对另一个逻辑 eraseblock 执行 markbad；
3. 记录 stats 和校验数据；
4. 正常 poweroff。

第二次复用同一镜像：

1. 验证原数据一致；
2. 注入单 data page loss并验证持久 parity recovery；
3. 验证 `_block_isbad()` 仍返回 1；
4. 验证 bad block write/erase 被拒绝；
5. 验证已写 block 从恢复的物理 `next_prog_page` 继续严格升序编程；
6. 执行既有 cancel、generation、serial smoke 回归。

## 12. 非目标

- QEMU 异常终止或主机掉电时的事务日志；
- 镜像在线升级或格式转换；
- QEMU migration/snapshot；
- factory bad-block 随机注入策略；
- 坏块替换和 reserve block 分配；
- FTL、wear leveling 或 GC；
- multi-plane/multi-die 并行 program；
- MTD、UBI、UBIFS core 修改。
