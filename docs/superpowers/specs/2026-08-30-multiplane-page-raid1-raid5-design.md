# Multi-plane Page-RAID1/RAID5 设计

## 1. 目标

在现有 `q3n-nand` QEMU 物理 NAND 模型和 Linux direct MTD 驱动上，增加同一 die 内基于多 plane 命令的 Page-RAID1 与 Page-RAID5。

固定拓扑为 2 die × 4 plane：

- RAID1：每个 die 内 `plane0 + plane1`、`plane2 + plane3` 分别组成双副本镜像组。
- RAID5：每个 die 内四个 plane 组成独立的 `3 data + 1 rotating parity` stripe。
- 两个 die 只承载不同的逻辑擦除块，不互相保存冗余副本或 parity。

Linux 驱动通过只读启动参数 `raid_level=1|5` 选择 profile，默认值为 RAID5。启动后不允许在线切换 RAID level，同一介质镜像不能混用两种布局。

## 2. 基线

实现基于分支 `codex/page-raid-serial-async-priority` 的提交 `6a687d8`，新分支为 `codex/multiplane-page-raid1-raid5`。

当前 QEMU 已声明 2 die × 4 plane、每 plane 247 blocks、每 block 1600 pages、每 page 16 KiB main data，并具有 LDPC、logical OOB、坏块标记、持久化介质和物理地址故障注入。当前控制器 ABI 只能一次提交一个物理 page。当前 Linux 驱动使用同一物理 block 内的串行 `D0..D6,P` 布局，不是真正的 multi-plane 提交。

新实现用 RAID profile 映射替代串行 `D0..D6,P` 布局。已有介质格式中的物理 page 布局不变，但旧布局数据不与新 profile 兼容；验收使用 fresh NAND image。

## 3. 非目标

- 不跨 die 建立镜像或 parity stripe。
- 不实现 RAID level 在线切换、同一 MTD 内混合 RAID level 或旧布局迁移。
- 不实现 FTL、GC、磨损均衡、坏块替换映射或后台重建。
- 不实现两个及以上成员同时失效时的数据恢复。
- 不修改 MTD core、UBI 或 UBIFS。
- 不宣称 QEMU 是时序精确的 ONFI multi-plane 模型；本阶段验证命令组织、地址约束、逐 plane 状态和驱动恢复语义。
- RAID5 不兼容 16 KiB 小写；最小写入粒度固定为 48 KiB。

## 4. 组件边界

QEMU 控制器只提供通用的同 die multi-plane read、program、OOB program 和 erase，不识别 RAID1、RAID5、镜像、parity 或 MTD 语义。

Linux 驱动负责：

- RAID level 参数和 profile 选择；
- 逻辑地址到 die、plane、block、page 的映射；
- RAID5 XOR parity；
- 两阶段提交和 manifest；
- LDPC 结果聚合、镜像回退和 RAID5 重建；
- 逻辑坏块传播、MTD geometry 和 debugfs 统计。

MTD core、UBI 和 UBIFS 不新增 RAID 相关分支。

## 5. QEMU multi-plane ABI

### 5.1 Capability 与 staging

共享的 QEMU/Linux 头文件增加 `Q3N_CAP_MULTIPLANE`。驱动 probe 时必须检查该 capability；缺失时 `raid_level=1` 和 `raid_level=5` 均拒绝注册，不能静默退回串行命令。

控制器提供四个 slot，slot 编号固定等于 die 内 plane 编号 0..3。新增寄存器保存：

- 目标 die；
- plane enable bitmap；
- 当前 staging slot；
- 当前 slot 的物理地址；
- 命令完成后的 success bitmap 和 failure bitmap；
- 每个 slot 独立的 ECC status、max bitflips、corrected bits 和 failed step。

选择 staging slot 后，现有 PIO data window 只访问该 slot 的 buffer。设置 main 或 OOB 长度会重置当前 slot 的 staging cursor，不得清除其他 slot 已准备的数据。

### 5.2 命令

新增以下通用命令：

- multi-plane main read；
- multi-plane main program；
- multi-plane logical-OOB read；
- multi-plane logical-OOB program；
- multi-plane block erase。

命令开始前一次性验证全部 enabled slot：

- plane bitmap 非零且只使用低四位；
- 每个 slot 的物理地址解码到指定 die 和对应 plane；
- enabled plane 不重复；
- 同一命令的 `block_in_plane` 和 page row 相同；
- erase 的 page 和 column 为零；
- program 的 staging 长度完整；
- 地址、长度和介质索引无溢出。

静态 descriptor 验证失败时不修改任何介质，所有 enabled slot 标记失败。descriptor 有效后，介质执行仍允许某一个 plane 因 program/erase fault 失败，其他 plane 可以成功；逐 slot bitmap 必须准确反映这种部分完成。

main program 只修改 main 与控制器 LDPC 区并保留 logical OOB。OOB program 只修改 128 B logical OOB 并保留 main 与 LDPC。这一分离用于 Linux 的两阶段提交。

### 5.3 物理 lane 编号

物理 block 保持扁平编号，但两侧统一使用显式公式：

```text
lane           = die * 4 + plane
physical_block = lane * blocks_per_plane + block_in_plane
physical_page  = physical_block * pages_per_block + page
```

任何 RAID 映射都不能依赖若干 data blocks 在扁平地址中偶然连续。

## 6. RAID1 profile

### 6.1 Geometry 与映射

RAID1 的 MTD geometry 为：

```text
writesize = 16 KiB
erasesize = pages_per_block * 16 KiB
size      = 2 dies * 2 mirror pairs * data_blocks_per_plane * erasesize
```

对逻辑擦除块 `leb`：

```text
mirror_set    = leb % 4
die           = mirror_set / 2
pair          = mirror_set % 2
primary_plane = pair * 2
mirror_plane  = primary_plane + 1
block_in_plane = leb / 4
```

一个 logical eraseblock 的所有 page 都留在同一个 die 和同一个 mirror pair。一次 16 KiB 写使用包含两个 plane 的 multi-plane main program。

### 6.2 读取与恢复

驱动按 page/请求序号在两份副本间轮换首选 plane，避免正常读流量永久集中在一个副本。

首选副本按 LDPC 和 manifest data CRC 验证：

- clean/corrected：直接返回，并累计真实 LDPC corrected bits；
- uncorrectable 或 CRC 不匹配：读取另一副本；
- 另一副本有效：返回其数据，将结果至少提升到 `mtd->bitflip_threshold`，形成 `-EUCLEAN`，并增加 `raid_recovered`；
- 两份副本均无效：增加一次 `mtd->ecc_stats.failed` 和 `raid_failed`，返回 `-EBADMSG`。

## 7. RAID5 profile

### 7.1 Geometry 与 parity 轮转

RAID5 的 MTD geometry 为：

```text
writesize = 3 * 16 KiB = 48 KiB
erasesize = pages_per_block * 48 KiB
size      = 2 dies * data_blocks_per_plane * erasesize
```

对逻辑擦除块 `leb`：

```text
die            = leb % 2
block_in_plane = leb / 2
```

擦除块内每个 48 KiB 单元对应一个物理 page row。全局 stripe 编号固定为
`stripe_id = leb * pages_per_block + page_row`，不能使用运行期递增计数器。对该
`stripe_id`：

```text
parity_plane = stripe_id % 4
data planes  = 其余 plane，按 plane 编号升序映射 D0、D1、D2
parity       = D0 XOR D1 XOR D2
```

一次对齐的 48 KiB 写先在 RAM 中计算 16 KiB parity，再用四 slot multi-plane main program 提交。驱动不跨请求缓存不完整 stripe；非 48 KiB 对齐或长度不是 48 KiB 整数倍的写返回 `-EINVAL`。

### 7.2 读取与恢复

普通读取只访问请求覆盖的 data plane，并验证 LDPC 与 manifest data CRC。目标 data page 不可纠时，驱动通过 multi-plane read 读取 parity 和另外两个 data 成员：

```text
target = parity XOR other_data_0 XOR other_data_1
```

只有三个来源成员都可读、manifest/generation 一致、parity CRC 有效且目标 data CRC 匹配时，恢复才成功。成功结果形成 `-EUCLEAN` 并增加 `raid_recovered`。任一必要来源不可纠、manifest 冲突或重建 CRC 错误都返回 `-EBADMSG`，并各增加一次标准 failed 与 `raid_failed`。

若目标 data 自身有效，即使另一个成员已退化，也允许返回目标数据；但只有在实际需要重建目标时才读取并要求其余三个成员健康。

## 8. 两阶段提交与 manifest

### 8.1 Manifest v2

RAID1 和 RAID5 共用新的 v2 manifest。其内容必须能放入 128 B logical OOB，并包含：

- magic 与 format version；
- RAID level；
- die 与 plane/member bitmap；
- stripe ID 与非零 generation；
- RAID1 mirror pair 或 RAID5 parity plane；
- data page 数量；
- 每个逻辑 data page 的 CRC；
- RAID5 parity CRC；
- header CRC。

同一 RAID group 的 enabled 成员保存字节完全相同的 manifest。manifest 不依赖成员自身 OOB 中的隐式 slot 顺序，成员角色由 manifest 和物理 plane 共同推导。运行期和重启扫描建立的 committed index 保存从任一有效成员取得的权威 manifest；metadata-degraded 时，普通读取不要求目标 data page 自己也持有 manifest。

### 8.2 写入顺序

写入固定分两阶段：

1. 对所有 RAID 成员执行 multi-plane main program，暂不写 commit manifest。
2. 读取逐 slot 状态；任一必需 main slot 失败时停止，返回 `-EIO`，不执行 OOB commit。
3. main 全部成功后，构造完整 manifest，并向所有成员执行 multi-plane OOB program。
4. 至少一个成员成功保存 manifest 时写请求成功；manifest 未覆盖全部成员则增加 `metadata_degraded`。
5. 所有 manifest slot 都失败时返回 `-EIO`，该 stripe 不发布。

任何有效 manifest 都证明驱动在发出 OOB commit 前已观察到全部 main slot 成功。掉电发生在 main 和 OOB 之间时没有 manifest，重启后该 stripe 不可见。多个可读 manifest 内容不一致时 stripe 为 corrupt，不选择其中任意一个继续读取。

### 8.3 重启恢复

probe 扫描 RAID profile 对应的 data pool OOB：

- 全 `0xff`：empty；
- 没有有效 manifest：uncommitted，不发布；
- 至少一个有效 manifest且其余有效副本完全一致：committed；
- 有效 manifest 相互冲突、成员/物理位置不匹配或 generation 混杂：corrupt。

每个 block group 的 RAM generation 从扫描到的最大一致非零 generation 恢复。成功 erase 后 generation 递增并清除该 group 的索引；同步 multi-plane 路径不保留跨 erase 的后台 parity worker。

## 9. 擦除与坏块

RAID1 erase 同时作用于 mirror pair 的两个 block；RAID5 erase 同时作用于同一 die 的四个对应 block。任一 slot erase 失败时逻辑 erase 返回失败。

`_block_isbad` 对 RAID group 的所有成员读取 BBM，任一成员为 bad 即报告整个 logical eraseblock bad。`_block_markbad` 在 group 的所有成员写 BBM，使逻辑坏块状态跨重启一致。首版不从 reserve pool 分配替换 block，也不自动把一次注入的 program failure 转换为永久 BBM。

erase、markbad 和 remove 与 MTD I/O 共用现有互斥边界；multi-plane 命令完成前不得释放锁或变更 group generation。

## 10. MTD 返回值与统计

保留标准 MTD 统计：

- `mtd->ecc_stats.corrected` 只增加硬件模型报告的真实 LDPC corrected bits；
- 只有最终无法提供正确逻辑数据时才增加 `mtd->ecc_stats.failed`；
- RAID 恢复不把 page 大小伪装成 corrected bits；恢复成功通过 `bitflip_threshold` 形成 `-EUCLEAN`。

debugfs 新增或保留：

```text
raid_level
multiplane_commands
multiplane_slot_failures
metadata_degraded
raid_recovered
raid_failed
raid_source_corrected_bits
```

QEMU 继续按物理操作统计 page programs、reads、erases、LDPC corrected/uncorrectable 和 fault injection，并新增 multi-plane command 与逐 slot failure 计数。一个四 slot 命令增加一次 multi-plane command，同时按实际执行成员增加物理操作计数。

## 11. 错误处理矩阵

| 场景 | 写/读结果 | 持久状态 |
| --- | --- | --- |
| 所有 main 与 manifest slot 成功 | success | committed/protected |
| main program 任一 slot 失败 | `-EIO` | 无 manifest，uncommitted |
| main 全成功，部分 manifest slot 失败 | success | committed，metadata-degraded |
| main 全成功，全部 manifest slot 失败 | `-EIO` | uncommitted |
| RAID1 首选副本失败，镜像有效 | `-EUCLEAN` | committed/degraded |
| RAID1 两副本均失败 | `-EBADMSG` | unreadable |
| RAID5 目标失败，其余三成员有效 | `-EUCLEAN` | committed/degraded |
| RAID5 重建来源也有失败 | `-EBADMSG` | unreadable target |
| Manifest CRC/位置/generation 冲突 | `-EBADMSG` | corrupt |
| Erase 任一 slot 失败 | `-EIO` | logical block failed |
| 任一成员 BBM 为 bad | block is bad | whole RAID group bad |

## 12. 文件边界

预计修改：

- `qemu/include/hw/mtd/q3n-nand.h`：共享 QEMU 侧 multi-plane ABI；
- `qemu/hw/mtd/q3n-nand.c`：四 slot staging、命令验证、执行和状态；
- `linux/drivers/mtd/nand/raw/qemu_3dnand.h`：Linux 侧相同 ABI；
- `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`：RAID profile、地址和 manifest 类型；
- `linux/drivers/mtd/nand/raw/qemu_3dnand_map.c`：RAID1/RAID5 纯映射；
- `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`：manifest、XOR、镜像选择和恢复纯逻辑；
- `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`：参数、MTD callbacks、multi-plane 提交、恢复编排与统计；
- `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`：纯逻辑测试；
- `scripts/smoke-test.sh`、guest profile 脚本和 host wrapper：结构及端到端测试；
- `README.md`、`qemu/README.md`：配置、布局、命令与限制。

现有未跟踪 `.vscode/` 不读取、不修改、不提交。

## 13. 验证矩阵

### 13.1 KUnit

- RAID1 四个 logical mirror set 到 die/pair/block/page 的首尾映射；
- RAID5 两个 die 的 logical eraseblock 分布；
- RAID5 parity 在 plane 0、1、2、3 间轮转且 D0..D2 顺序稳定；
- 非 48 KiB RAID5 写对齐拒绝；
- v2 manifest round trip、header CRC、位置、member bitmap 和 generation 验证；
- RAID1 单副本恢复、双副本失败；
- RAID5 任意 data slot XOR 恢复、parity CRC、目标 CRC 和双故障拒绝；
- MTD size/erasesize 边界与整数溢出检查。

### 13.2 QEMU/ABI

- Linux 与 QEMU 寄存器、命令、capability 和位图常量一致；
- 同 die 两 slot 和四 slot main/OOB/read/erase 成功；
- 跨 die、地址 plane 与 slot 不匹配、不同 block/page row、重复或空 plane bitmap 被拒绝且介质不变；
- 指定一个物理地址的 program fault 只设置对应 slot failure；
- 每个 slot 的 LDPC 结果独立锁存；
- main program 保留 OOB，OOB program 保留 main 与 LDPC。

### 13.3 Guest 端到端

RAID1：

- `raid_level=1` geometry 为 16 KiB writesize 和双副本容量；
- 多个 mirror set 跨两个 die 分布但副本保持同 die；
- 正常写读、擦除和重启恢复；
- 首选副本 41-bit LDPC failure 后镜像恢复并形成 `-EUCLEAN`；
- 两副本均失败时 `-EBADMSG`；
- 单成员 BBM 使整个 logical eraseblock bad。

RAID5：

- `raid_level=5` geometry 为 48 KiB writesize 和 75% 容量；
- 连续四个 stripe 的 parity plane 依次为 0、1、2、3；
- 48 KiB 数据写读和重启恢复；
- D0、D1、D2 任意一个发生 41-bit failure 时可恢复并形成 `-EUCLEAN`；
- parity failure 后健康目标 data 仍可读，但失效 data 无法重建；
- 目标加任一必要来源双故障返回 `-EBADMSG`；
- main 部分 program failure 不生成 manifest，重启后不发布；
- 部分 OOB commit 成功增加 `metadata_degraded` 且数据可见。

### 13.4 回归

- 本地结构 smoke；
- QEMU overlay build；
- Linux overlay/KUnit build；
- 基础 MTD smoke；
- LDPC 39/40/41-bit 边界；
- logical OOB 独立编程；
- 坏块标记 main preservation；
- sparse NAND image 持久化。

## 14. 完成标准

- `raid_level=1` 和 `raid_level=5` 均能在 fresh NAND 上注册正确 geometry 的 MTD；
- 所有冗余成员位于同一 die 的不同 plane；
- RAID1 使用双 plane program，RAID5 使用四 plane program 和 rotating parity；
- 两阶段提交阻止 main 部分完成的 stripe 在重启后可见；
- 单成员读取失败可按 profile 恢复，双故障不会返回未经验证的数据；
- 标准 ECC 统计、debugfs 统计和逐 slot QEMU 状态符合本设计；
- KUnit、结构检查、QEMU/Linux 构建、RAID1/RAID5 guest smoke 与既有关键回归全部通过。
