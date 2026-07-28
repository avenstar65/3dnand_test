# QEMU 3D NAND 寄存器参考

本文档描述当前 `q3n-nand` QEMU 模型实际实现的 MMIO 寄存器接口。

## 1. 接口概览

- PCI Vendor ID：`0x1b36`
- PCI Device ID：`0x003d`
- PCI Revision：`0x01`
- MMIO 空间大小：`0x10000`
- 字节序：Little Endian
- 支持的 MMIO 访问宽度：1～4 字节
- 物理 main page：16 KiB
- 物理 OOB：1664 B
- Linux 可见的逻辑 OOB：1024 B
- ECC step：1024 B
- ECC 强度：40 bit/step
- LDPC 数据：96 B/step，共 16 step

物理地址寄存器使用 16 KiB main page 作为寻址步长，而不是使用
`16 KiB + 1664 B` 的后端物理槽位大小。

- 页操作要求地址按 16 KiB 对齐。
- 块操作要求地址按 erase block 对齐。
- 物理 OOB 和 LDPC 区域由 QEMU 控制器内部映射，驱动不能通过地址寄存器直接访问。

## 2. 寄存器总表

| 偏移 | 寄存器 | 属性 | 说明 |
| ---: | --- | :---: | --- |
| `0x0000` | `Q3N_REG_ID` | RO | 控制器标识，返回 `0x314e3351`（`Q3N1`） |
| `0x0004` | `Q3N_REG_CAP` | RO | 控制器能力位 |
| `0x0008` | `Q3N_REG_CTRL` | WO | 控制寄存器，bit0 写 1 执行 RESET |
| `0x000c` | `Q3N_REG_STATUS` | RO | 控制器状态 |
| `0x0010` | `Q3N_REG_CMD` | RW | 写入并执行命令；读取返回最后命令 |
| `0x0014` | `Q3N_REG_ADDR_LO` | RW | NAND 物理字节地址低 32 位 |
| `0x0018` | `Q3N_REG_ADDR_HI` | RW | NAND 物理字节地址高 32 位 |
| `0x001c` | `Q3N_REG_LEN` | RW | main 数据传输长度；写入时复位 PIO buffer |
| `0x0020` | `Q3N_REG_GEOM0` | RO | page size 和逻辑 OOB 大小 |
| `0x0024` | `Q3N_REG_GEOM1` | RO | pages/block 和 blocks/plane |
| `0x0028` | `Q3N_REG_POOL0` | RO | data/parity pool 大小 |
| `0x002c` | `Q3N_REG_POOL1` | RO | metadata/reserve pool 大小 |
| `0x0030` | `Q3N_REG_OOB_LEN` | RW | 独立 logical OOB 传输长度；写入时复位 PIO buffer |
| `0x0034` | `Q3N_REG_OP_CLASS` | RW | 前台、parity read 或 parity write 分类 |
| `0x0038` | `Q3N_REG_IRQ_STATUS` | RW1C | 中断状态，写 1 清除 |
| `0x003c` | `Q3N_REG_IRQ_MASK` | RW | 中断使能掩码 |
| `0x0040` | `Q3N_REG_STAT_PAGE_PROGRAMS` | RO | 成功的物理页编程次数 |
| `0x0044` | `Q3N_REG_STAT_BLOCK_ERASES` | RO | 成功的物理块擦除次数 |
| `0x0048` | `Q3N_REG_STAT_PAGE_READ_ERRORS` | RO | 后端介质读取失败次数 |
| `0x005c` | `Q3N_REG_STAT_FAULTS_INJECTED` | RO | 已执行或已触发的故障注入次数 |
| `0x0060` | `Q3N_REG_FAULT_ADDR_LO` | RW | 故障目标物理地址低 32 位 |
| `0x0064` | `Q3N_REG_FAULT_ADDR_HI` | RW | 故障目标物理地址高 32 位 |
| `0x0068` | `Q3N_REG_FAULT_CTRL` | RW/触发 | 执行或布置故障注入 |
| `0x0070` | `Q3N_REG_STAT_FG_OPS` | RO | 前台操作次数 |
| `0x0074` | `Q3N_REG_STAT_PARITY_READS` | RO | parity read 次数 |
| `0x0078` | `Q3N_REG_STAT_PARITY_WRITES` | RO | parity write 次数 |
| `0x0080` | `Q3N_REG_BLOCK_STATUS` | RO | 最近一次块状态查询结果 |
| `0x0088` | `Q3N_REG_ECC_GEOM0` | RO | ECC step size 和 strength |
| `0x008c` | `Q3N_REG_ECC_GEOM1` | RO | LDPC bytes/step 和 steps/page |
| `0x0090` | `Q3N_REG_ECC_STATUS` | RO | 最近一次页读取的 ECC 状态 |
| `0x0094` | `Q3N_REG_ECC_MAX_BITFLIPS` | RO | 单个 step 最大已纠正 bitflip 数 |
| `0x0098` | `Q3N_REG_ECC_CORRECTED_BITS` | RO | 当前页累计已纠正 bit 数 |
| `0x009c` | `Q3N_REG_ECC_FAILED_STEP` | RO | 第一个不可纠正的 ECC step |
| `0x00a0` | `Q3N_REG_FAULT_STEP` | RW | 故障注入目标 ECC step |
| `0x00a4` | `Q3N_REG_FAULT_FIRST_BIT` | RW | step 内第一个故障 bit |
| `0x00a8` | `Q3N_REG_FAULT_COUNT` | RW | 注入 bitflip 数量 |
| `0x00ac` | `Q3N_REG_FAULT_REGION` | RW | 注入 main 或 LDPC 区域 |
| `0x00b0` | `Q3N_REG_STAT_LDPC_CORRECTED` | RO | 累计纠正 bit 数 |
| `0x00b4` | `Q3N_REG_STAT_LDPC_UNCORRECTABLE` | RO | 累计不可纠正页数 |
| `0x00b8` | `Q3N_REG_STAT_LDPC_FAILED_STEPS` | RO | 累计不可纠正 step 数 |
| `0x1000` | `Q3N_REG_DATA` | RW | 流式 PIO 数据窗口 |

未定义的寄存器偏移读取返回 0，写入被忽略。

## 3. 控制器能力

`Q3N_REG_CAP` 当前固定返回 `0x00000007`。

| 位 | 定义 | 说明 |
| ---: | --- | --- |
| 0 | `Q3N_CAP_BASIC_FLASH` | 支持基础 NAND 读、写、擦除 |
| 1 | `Q3N_CAP_PERSISTENT_MEDIA` | 支持持久化 NAND 后端镜像 |
| 2 | `Q3N_CAP_BAD_BLOCK_MARKER` | 支持物理坏块标记 |

## 4. 状态寄存器

`Q3N_REG_STATUS` 位定义：

| 位 | 定义 | 说明 |
| ---: | --- | --- |
| 0 | `Q3N_STATUS_READY` | 命令已经完成 |
| 1 | `Q3N_STATUS_ERROR` | 命令、地址、长度或介质操作失败 |
| 2 | `Q3N_STATUS_ECC_UNCORRECTABLE` | 最近一次页读取存在不可纠正 LDPC 错误 |

开始执行命令时，控制器清除 `READY` 和普通 `ERROR`。命令完成后重新置
`READY`。新的 main 页读取或 RESET 会清除上一笔 ECC 结果；OOB-only
读取不改变 ECC 结果。

不可纠正 LDPC 错误使用 `Q3N_STATUS_ECC_UNCORRECTABLE` 和 ECC 结果寄存器
报告，不等同于底层介质 I/O 失败。

## 5. 命令寄存器

向 `Q3N_REG_CMD` 写入以下命令码会立即执行命令。

| 命令码 | 名称 | 说明 |
| ---: | --- | --- |
| 0 | `Q3N_CMD_NOP` | 空操作 |
| 1 | `Q3N_CMD_READ_ID` | 将 8 字节 NAND ID 放入 PIO buffer |
| 2 | `Q3N_CMD_READ_PAGE` | 读取 16 KiB main，并执行 LDPC 模拟 |
| 3 | `Q3N_CMD_PROGRAM_PAGE` | 编程 main，由 QEMU 自动生成 LDPC |
| 4 | `Q3N_CMD_ERASE_BLOCK` | 擦除物理块及其 bitflip overlay |
| 5 | `Q3N_CMD_RESET` | 复位命令、PIO、IRQ、ECC 和一次性 PROGRAM 故障状态 |
| 6 | `Q3N_CMD_READ_PAGE_OOB` | 只读取 1024 B logical OOB |
| 7 | `Q3N_CMD_PROGRAM_PAGE_OOB` | 只编程 1024 B logical OOB，保留 main 和 LDPC |
| 8 | `Q3N_CMD_GET_BLOCK_STATUS` | 查询目标物理块的 BBM |

### 5.1 READ_ID 返回值

`READ_ID` 在 PIO buffer 中返回：

```text
2c d7 90 a6 51 33 4e 44
```

后四个字节的 ASCII 表示为 `Q3ND`。

### 5.2 RESET

以下两种操作等价：

```text
Q3N_REG_CTRL.bit0 = 1
Q3N_REG_CMD = Q3N_CMD_RESET
```

RESET 会：

- 解除尚未触发的 `FAIL_NEXT_PROGRAM`；
- 清除普通错误状态；
- 将最后命令设为 `NOP`；
- 清空 PIO buffer 游标和有效长度；
- 清除 IRQ 状态并拉低 IRQ；
- 清除最近一次 ECC 结果；
- 产生一次新的命令完成状态。

RESET 不清零累计统计寄存器。

## 6. 地址与传输长度

### 6.1 物理地址

`ADDR_HI:ADDR_LO` 组成 64 位 NAND 物理字节地址：

```text
physical_page = address / 16384
column        = address % 16384
block         = physical_page / 1600
page          = physical_page % 1600
```

当前控制器只接受 page column 为 0 的页操作。

`ERASE_BLOCK` 和 `GET_BLOCK_STATUS` 还要求地址位于物理 erase block 的
起始位置。

### 6.2 LEN

写 `Q3N_REG_LEN` 会：

- 更新 main 数据长度；
- 将 PIO `data_pos` 复位为 0；
- 将 PIO `data_count` 清零。

当前 PROGRAM 命令的实际长度要求为：

| 命令 | PIO buffer 最小有效数据 |
| --- | ---: |
| `PROGRAM_PAGE` | 16384 B |

### 6.3 OOB_LEN

独立 logical OOB 命令要求：

```text
Q3N_REG_OOB_LEN = 1024
```

写 `OOB_LEN` 也会把 PIO `data_pos` 和 `data_count` 复位为 0。随后命令 6
返回恰好 1024 B，命令 7 至少需要 1024 B 有效 PIO 数据；两者均不读取或写入
main，也不生成、解码或清除 LDPC/ECC 结果。长度不为 1024 时命令返回
`STATUS.ERROR`。

## 7. 几何寄存器

### 7.1 GEOM0

```text
bits [15:0]  = page size
bits [31:16] = logical OOB size
```

当前值：

```text
Q3N_REG_GEOM0 = 0x00804000
```

表示：

- page size：`0x4000` = 16384 B；
- logical OOB：`0x80` = 1024 B。

### 7.2 GEOM1

```text
bits [15:0]  = pages per block
bits [31:16] = blocks per plane
```

当前值：

```text
Q3N_REG_GEOM1 = 0x00f70640
```

表示：

- 1600 pages/block；
- 247 blocks/plane。

模型包含 2 个 die，每个 die 有 4 个 plane，共 8 个物理 lane。

## 8. Block Pool

### 8.1 POOL0

```text
bits [15:0]  = data blocks per plane
bits [31:16] = parity blocks per plane
```

默认值：

```text
Q3N_REG_POOL0 = 0x002000d0
```

- data：208 blocks/plane；
- parity：32 blocks/plane。

### 8.2 POOL1

```text
bits [15:0]  = metadata blocks per plane
bits [31:16] = reserve blocks per plane
```

默认值：

```text
Q3N_REG_POOL1 = 0x00040003
```

- metadata：3 blocks/plane；
- reserve：4 blocks/plane。

## 9. 操作分类

`Q3N_REG_OP_CLASS` 只接受以下值：

| 值 | 定义 | 说明 |
| ---: | --- | --- |
| 0 | `Q3N_OP_FOREGROUND` | 普通前台 MTD 操作 |
| 1 | `Q3N_OP_PARITY_READ` | RAID parity read |
| 2 | `Q3N_OP_PARITY_WRITE` | RAID parity write |

超出范围的写入被忽略。

该寄存器只影响统计分类，不改变 NAND 介质读写语义。

## 10. IRQ

`Q3N_REG_IRQ_STATUS` 和 `Q3N_REG_IRQ_MASK` 使用以下位：

| 位 | 定义 | 说明 |
| ---: | --- | --- |
| 0 | `Q3N_IRQ_DONE` | 命令完成 |
| 1 | `Q3N_IRQ_ERROR` | 命令失败 |

行为：

- 任意命令结束都会置 `DONE`；
- 命令失败同时置 `DONE` 和 `ERROR`；
- `IRQ_STATUS & IRQ_MASK` 非零时拉高 IRQ；
- 向 `IRQ_STATUS` 写 1 清除相应状态位；
- 清除所有已使能状态后拉低 IRQ。

## 11. 坏块状态

执行 `GET_BLOCK_STATUS` 后，结果锁存在 `Q3N_REG_BLOCK_STATUS`。

| 位 | 定义 | 当前语义 |
| ---: | --- | --- |
| 0 | `Q3N_BLOCK_STATUS_BAD` | 物理块首页 BBM 非 `0xff` |
| 1 | `Q3N_BLOCK_STATUS_ERASED` | 头文件保留，但当前实现不返回 |

当前 QEMU 不维护每页是否已经编程的状态，所以块状态只反映 BBM，不反映
“块是否擦除”。

Linux `_block_markbad` 不使用专用控制器命令。它构造 1024 B logical OOB
（全部为 `0xff`，byte 0 为 `0x00`），并对块首页执行
`PROGRAM_PAGE_OOB`。因此标坏与普通 OOB 编程共享同一条 NAND
bytewise-AND 路径；重复标坏是幂等操作，main 和 LDPC 保持不变。

## 12. ECC/LDPC 几何

### 12.1 ECC_GEOM0

```text
bits [15:0]  = ECC step size
bits [31:16] = ECC strength
```

当前值：

```text
Q3N_REG_ECC_GEOM0 = 0x00280400
```

- step size：1024 B；
- strength：40 bit/step。

### 12.2 ECC_GEOM1

```text
bits [15:0]  = LDPC bytes per step
bits [31:16] = LDPC steps per page
```

当前值：

```text
Q3N_REG_ECC_GEOM1 = 0x00100060
```

- 96 B LDPC/step；
- 16 steps/page；
- 每页共 1536 B LDPC。

## 13. ECC 读取结果

每次 `READ_PAGE` 开始前，控制器清除旧 ECC 结果，读取完成后锁存：

| 寄存器 | 说明 |
| --- | --- |
| `ECC_STATUS` | clean、corrected 或 uncorrectable |
| `ECC_MAX_BITFLIPS` | 所有 step 中最大的可纠正 bitflip 数 |
| `ECC_CORRECTED_BITS` | 所有可纠正 step 的 bitflip 总数 |
| `ECC_FAILED_STEP` | 第一个不可纠正 step |

`ECC_STATUS` 编码：

| 值/位 | 定义 |
| ---: | --- |
| 0 | `Q3N_ECC_STATUS_CLEAN` |
| bit0 | `Q3N_ECC_STATUS_CORRECTED` |
| bit1 | `Q3N_ECC_STATUS_UNCORRECTABLE` |

没有失败 step 时：

```text
Q3N_REG_ECC_FAILED_STEP = 0xffffffff
```

每个 step 的 main overlay 与 LDPC overlay bitflip 数相加：

- `0`：clean；
- `1～40`：可纠正；
- `>40`：不可纠正；
- 保存的 LDPC 与根据 main 数据重新生成的 LDPC 不一致：不可纠正。

不可纠正时，QEMU 将 main overlay 应用到返回数据，以便 Linux/RAID 层看到
损坏数据，同时置 `STATUS.ECC_UNCORRECTABLE`。

## 14. 故障注入

### 14.1 FAULT_CTRL

| 位 | 定义 | 说明 |
| ---: | --- | --- |
| 0 | `Q3N_FAULT_INJECT_DATA_LOSS` | 向 main step 0 注入 41 bit 错误 |
| 1 | `Q3N_FAULT_FAIL_NEXT_PROGRAM` | 指定地址的下一次 PROGRAM 失败 |
| 2 | `Q3N_FAULT_INJECT_BITFLIPS` | 使用详细参数注入 bitflip |

每次写 `FAULT_CTRL` 都会先撤销之前尚未触发的
`FAIL_NEXT_PROGRAM`，然后按照新值执行或布置故障。

写 0 可以显式撤销一次性 PROGRAM 故障。

### 14.2 DATA_LOSS

`Q3N_FAULT_INJECT_DATA_LOSS` 固定向目标页的：

- step 0；
- main 区域；
- 从 bit 0 开始；
- 注入 41 bit。

因为 ECC strength 为 40，所以后续读取一定报告不可纠正。

### 14.3 精细 bitflip 注入

`Q3N_FAULT_INJECT_BITFLIPS` 使用：

| 寄存器 | 说明 |
| --- | --- |
| `FAULT_STEP` | ECC step，范围 0～15 |
| `FAULT_FIRST_BIT` | 区域内第一个 bit |
| `FAULT_COUNT` | 连续注入的 bit 数 |
| `FAULT_REGION` | main 或 LDPC |

`FAULT_REGION`：

| 值 | 区域 |
| ---: | --- |
| 0 | `Q3N_FAULT_REGION_MAIN` |
| 1 | `Q3N_FAULT_REGION_LDPC` |

bitflip 保存在持久 overlay 中。对同一范围再次 XOR 注入会恢复原状；擦除
物理块会清除该块的全部 overlay。

### 14.4 下一次 PROGRAM 失败

`Q3N_FAULT_FAIL_NEXT_PROGRAM` 要求 `FAULT_ADDR`：

- 是合法物理页地址；
- column 为 0。

只有目标地址的下一次 PROGRAM 才会失败。触发后故障自动解除，并增加
`STAT_FAULTS_INJECTED`。

## 15. PIO 数据窗口

PIO 数据窗口范围：

```text
0x1000 ～ 0x507f
```

MMIO 窗口容量：

```text
16512 B
```

尽管窗口在 MMIO 中占据连续地址区间，当前实现使用内部 `data_pos` 顺序
读写，不会用本次 MMIO offset 作为 buffer 下标。因此驱动可以反复访问
`Q3N_REG_DATA`。单个命令不会组合 main 与 OOB：main 命令传 16384 B，
OOB 命令传 1024 B。

行为：

- 写 `LEN` 将游标和有效数据长度清零；
- PIO 写入按访问顺序追加到 buffer；
- PIO 读取按访问顺序从 buffer 取出；
- 超过有效读取长度的字节返回 `0xff`；
- 写出内部 buffer 边界会置 `STATUS.ERROR`。

命令对应的数据量：

| 命令 | PIO 数据 |
| --- | ---: |
| `READ_ID` | 8 B |
| `READ_PAGE` | 16384 B |
| `PROGRAM_PAGE` | 至少 16384 B |
| `READ_PAGE_OOB` | 1024 B |
| `PROGRAM_PAGE_OOB` | 至少 1024 B |

物理 OOB 中的 1536 B LDPC 不会出现在 PIO 窗口中。

## 16. 物理 OOB 与逻辑 OOB 映射

物理 page 的后端槽位总长为 `0x4a00`，布局：

```text
0x0000..0x3fff main
0x4000         OOB head / BBM / logical OOB[0]
0x4001..0x4600 LDPC
0x4601..0x49ff OOB tail / logical OOB[1..1023]
```

物理范围：

| 物理范围 | 大小 | 内容 |
| --- | ---: | --- |
| `0x4000` | 1 B | OOB head / BBM |
| `0x4001..0x4600` | 1536 B | 16 份 LDPC，每份 96 B |
| `0x4601..0x49ff` | 1023 B | OOB tail / 软件 metadata |

逻辑 OOB：

| 逻辑 OOB 范围 | 映射 |
| --- | --- |
| byte 0 | 物理 `0x4000`，即 BBM |
| bytes 1～1023 | 物理 `0x4601..0x49ff` |

因此：

- 软件读取逻辑 OOB 第一个字节时得到 BBM；
- 软件写逻辑 OOB 第一个字节可以标记坏块；
- LDPC 区域完全由 QEMU 控制器生成和检查；
- Linux 驱动不能直接读写物理 LDPC 字节；
- main PROGRAM 保留 `0x4000` 与 `0x4601..0x49ff`；
- OOB PROGRAM 保留 `0x0000..0x3fff` 与 `0x4001..0x4600`。

## 17. 统计寄存器说明

统计值在 QEMU 内部保存为 64 位，但 MMIO 只返回低 32 位。

| 寄存器 | 计数条件 |
| --- | --- |
| `STAT_PAGE_PROGRAMS` | 物理 PROGRAM 成功 |
| `STAT_BLOCK_ERASES` | 物理块 ERASE 成功 |
| `STAT_PAGE_READ_ERRORS` | 后端介质读取函数失败 |
| `STAT_FAULTS_INJECTED` | 即时故障注入成功或一次性 PROGRAM 故障触发 |
| `STAT_FG_OPS` | 前台 page read/program |
| `STAT_PARITY_READS` | `OP_CLASS=PARITY_READ` 的 page read |
| `STAT_PARITY_WRITES` | `OP_CLASS=PARITY_WRITE` 的 page program |
| `STAT_LDPC_CORRECTED` | 累计纠正 bit 数 |
| `STAT_LDPC_UNCORRECTABLE` | 累计不可纠正页数 |
| `STAT_LDPC_FAILED_STEPS` | 累计不可纠正 step 数 |

注意：`STAT_PAGE_READ_ERRORS` 不统计普通的 LDPC 不可纠正事件。LDPC 错误
使用独立的 `STAT_LDPC_*` 寄存器统计。

## 18. 当前实现边界

- QEMU 不维护每页 programmed/erased 状态。
- QEMU 不维护 `next_prog_page` 或块内编程顺序。
- 页可以任意、重复 PROGRAM。
- 重复 PROGRAM 使用 NAND 的逐字节 `old & incoming` 规则。
- 块内严格编程顺序由上层文件系统/软件保证。
- QEMU 不保存 RAID stripe、parity generation 或运行时 unprotected 状态。
- 当前 NAND Core 分支的 Linux 驱动不实现 RAID 布局、parity 调度或恢复。
- QEMU 中保留的 parity operation tag 仅是旧版统计 ABI，不构成 Page RAID
  实现，也没有需要跨 VM 重启恢复的 RAID 运行时状态。

## 19. 对应源码

- QEMU 寄存器与位定义：`qemu/include/hw/mtd/q3n-nand.h`
- QEMU MMIO 实现：`qemu/hw/mtd/q3n-nand.c`
- QEMU 持久介质实现：`qemu/hw/mtd/q3n-media.c`
- Linux 驱动寄存器定义：`linux/drivers/mtd/nand/raw/qemu_3dnand.h`
- Linux MTD 驱动实现：`linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
