# Q3N 独立 OOB 访问与坏块标记设计

## 1. 背景

当前 Q3N 控制器使用 `Q3N_CMD_READ_PAGE_OOB` 和
`Q3N_CMD_PROGRAM_PAGE_OOB` 一次传输 16 KiB main data 和 128 B logical
OOB。驱动的 `_block_markbad` 没有使用这条 OOB 路径，而是调用专用的
`Q3N_CMD_MARK_BAD_BLOCK`，QEMU 再通过 `q3n_media_mark_bad()` 旁路写入
BBM。

该实现存在两个问题：

1. 坏块标记有专用旁路，与“软件写 logical OOB byte 0 即标记坏块”的语义
   重复；
2. OOB 无法作为独立空间访问。OOB-only 写入会携带一页全 `0xff` main，
   QEMU 还会根据这份 main 重新生成 LDPC，可能在已经编程的页上额外改变
   LDPC 字节。

本次修改把 logical OOB 建模为可以独立读取和编程的控制器空间，并彻底
删除专用坏块标记命令。

## 2. 目标

- 物理 page 固定为 18048 B。
- 16 KiB main data 占据物理 page 的前 16384 B。
- BBM 固定位于物理 page offset `0x4000`，同时属于 logical OOB。
- logical OOB 共 128 B，在物理 page 中被 LDPC 区域分成 head 和 tail。
- QEMU OOB 命令只传输 logical OOB，不传输 main data。
- OOB-only 编程不修改 main data，也不修改或重新生成 LDPC。
- `_block_markbad` 通过 OOB-only PROGRAM 写 logical OOB byte 0。
- 从 QEMU、Linux 驱动和测试 ABI 中彻底删除
  `Q3N_CMD_MARK_BAD_BLOCK`。

## 3. 非目标

- 不修改物理 page 总大小。
- 不修改 LDPC step 数、每 step 字节数或纠错强度。
- 不恢复 VM 重启前的 RAID 运行时状态。
- 不在 QEMU 中增加页编程顺序或 `next_prog_page` 状态。
- 不改变 NAND 重复编程的 `old & incoming` 规则。
- 不删除 `Q3N_CMD_GET_BLOCK_STATUS`；块状态仍通过第一页 BBM 判断。

## 4. 物理 Page 布局

物理 page 使用以下固定布局：

```text
page offset 0x0000
┌─────────────────────────────────────────┐
│ Main data                               │
│ 16384 B                                 │
│ offsets 0x0000..0x3fff                  │
├─────────────────────────────────────────┤ 0x4000
│ Logical OOB head: BBM                   │
│ 1 B                                     │
│ offset 0x4000                           │
├─────────────────────────────────────────┤ 0x4001
│ Controller-owned LDPC                   │
│ 16 × 96 B = 1536 B                      │
│ offsets 0x4001..0x4600                  │
├─────────────────────────────────────────┤ 0x4601
│ Logical OOB tail: software metadata     │
│ 127 B                                   │
│ offsets 0x4601..0x467f                  │
└─────────────────────────────────────────┘ 0x4680
```

固定常量：

```text
Q3N_PAGE_SIZE                  = 0x4000
Q3N_PHYSICAL_OOB_HEAD_OFFSET  = 0x4000
Q3N_PHYSICAL_OOB_HEAD_SIZE    = 1
Q3N_PHYSICAL_LDPC_OFFSET      = 0x4001
Q3N_PHYSICAL_LDPC_SIZE        = 0x0600
Q3N_PHYSICAL_OOB_TAIL_OFFSET  = 0x4601
Q3N_PHYSICAL_OOB_TAIL_SIZE    = 0x007f
Q3N_PHYSICAL_PAGE_SIZE        = 0x4680
```

QEMU 和 Linux 共享头文件必须使用等价常量与编译期断言，确保：

```text
OOB_HEAD_OFFSET == PAGE_SIZE
OOB_HEAD_SIZE + OOB_TAIL_SIZE == LOGICAL_OOB_SIZE
LDPC_OFFSET == OOB_HEAD_OFFSET + OOB_HEAD_SIZE
OOB_TAIL_OFFSET == LDPC_OFFSET + LDPC_SIZE
PHYSICAL_PAGE_SIZE == OOB_TAIL_OFFSET + OOB_TAIL_SIZE
```

后端 image 中每个 page slot 的 stride 固定为
`Q3N_PHYSICAL_PAGE_SIZE`。稀疏 complemented encoding 不改变上述解码后的
物理字节顺序。

## 5. Logical OOB 映射

Linux 和控制器命令只看见 128 B logical OOB：

```text
logical OOB[0]
    <-> physical page[0x4000]
    <-> BBM

logical OOB[1..127]
    <-> physical page[0x4601..0x467f]
    <-> software metadata
```

物理 page `[0x4001..0x4600]` 的 1536 B LDPC 不进入 logical OOB 地址
空间。

QEMU 提供两个对称 helper：

- physical page -> logical OOB：收集 head 和 tail；
- logical OOB -> physical page：分别更新 head 和 tail。

helper 不读写 LDPC 区域。

## 6. 控制器命令语义

保留现有命令编号 6 和 7，但修改为 OOB-only：

| 命令 | 新语义 |
| --- | --- |
| `Q3N_CMD_READ_PAGE_OOB` | 只把 128 B logical OOB 放入 PIO buffer |
| `Q3N_CMD_PROGRAM_PAGE_OOB` | 只把 PIO 中的 128 B logical OOB 编程到物理 OOB head/tail |

### 6.1 READ_PAGE_OOB

调用要求：

- `ADDR` 指向合法且 page-aligned 的物理页；
- `OOB_LEN == 128`；
- main `LEN` 不参与该命令。

成功后：

- PIO `data_count == 128`；
- 第一个字节是 BBM；
- 后续 127 B 是 metadata；
- 不执行 LDPC decode；
- 不更新 `ECC_STATUS`；
- 按 `OP_CLASS` 更新 foreground/parity read 统计。

### 6.2 PROGRAM_PAGE_OOB

调用要求：

- `ADDR` 指向合法且 page-aligned 的物理页；
- `OOB_LEN == 128`；
- PIO 中至少有 128 B；
- main `LEN` 不参与该命令。

成功后：

- 只对 physical page offset `0x4000` 和
  `0x4601..0x467f` 执行 NAND `old & incoming`；
- 不读取或写回 main；
- 不生成 LDPC；
- 不修改现有 LDPC；
- 按 `OP_CLASS` 更新 foreground/parity write 统计；
- 计入一次成功的物理 PROGRAM。

写入全 `0xff` 的 logical OOB 字节不会改变已有物理位。

### 6.3 Main page 命令

`Q3N_CMD_READ_PAGE` 继续只读取 16 KiB main，并执行 LDPC decode。

`Q3N_CMD_PROGRAM_PAGE` 继续只从 PIO 接收 16 KiB main。QEMU 根据 main 和
page key 生成 1536 B LDPC，并在一次介质 PROGRAM 中写 main+LDPC；
logical OOB head/tail 使用全 `0xff`，因此不会改变已有 OOB 位。

需要同时写 main 和软件 metadata 时，驱动依次执行：

1. `PROGRAM_PAGE` 写 main 并生成 LDPC；
2. `PROGRAM_PAGE_OOB` 写 128 B logical OOB。

本阶段不承诺这两个命令跨崩溃原子。读取时现有 metadata CRC/manifest
校验负责拒绝不完整或不匹配的元数据。

## 7. 坏块标记

删除：

- `Q3N_CMD_MARK_BAD_BLOCK`；
- `q3n_cmd_mark_bad_block()`；
- `qemu_3dnand_mark_phys_block_bad_locked()`；
- `q3n_media_mark_bad()`；
- `q3n_media_write_bbm()`。

保留：

- `Q3N_CMD_GET_BLOCK_STATUS`；
- `q3n_media_get_block_status()`；
- 读取第一页 physical page offset `0x4000` 的 BBM。

驱动 `_block_markbad` 流程：

1. 验证逻辑块地址和 BBM capability；
2. 获取 `mtd_lock`；
3. 若块已经 bad，返回成功；
4. 取消并排空该块的 parity 工作；
5. 构造 128 B logical OOB，全填 `0xff`；
6. 设置 `logical_oob[0] = 0x00`；
7. 对该块第一页执行 OOB-only `PROGRAM_PAGE_OOB`；
8. 成功后设置驱动运行时 `data_meta[block].bad = true`；
9. 结束 parity cancel barrier。

该流程不会写 main，不会重新生成 LDPC，也不会创建第二份坏块状态。

普通 `_write_oob` 对第一页 logical OOB byte 0 写入任意非 `0xff` 值，仍然
会让后续 `GET_BLOCK_STATUS` 和 `_block_isbad` 报告坏块。

## 8. QEMU Media 接口

media 层增加独立操作：

```text
q3n_media_read_logical_oob()
q3n_media_program_logical_oob()
```

它们按固定 page-slot offset 直接访问 OOB head/tail，不把 1664 B spare
区域当作一段 guest-visible OOB。

`q3n_media_program_logical_oob()`：

- 先读取目标块第一页 BBM；
- 对已经 bad 的块，只有幂等地继续写同一 BBM 值可以直接返回成功；其他
  page/OOB 编程仍返回 `-EIO`；
- 对 head 和 tail 分别执行 `old & incoming`；
- 使用一次后端写入提交更新后的完整 physical page slot，避免 head 成功、
  tail 失败的半写状态；
- 成功后 flush 行为与现有 page PROGRAM 保持一致。

普通 `q3n_media_program_page()` 拆分为清晰的 main+LDPC 路径，不再把
logical OOB 当作参数。

## 9. Linux 驱动数据流

低层 helper 调整为：

```text
qemu_3dnand_read_phys_page_locked()
qemu_3dnand_program_phys_page_locked()
qemu_3dnand_read_phys_oob_locked()
qemu_3dnand_program_phys_oob_locked()
```

其中 OOB helper 只读写 128 B，PIO 不再搬运 16 KiB main。

上层行为：

- `_read`：只走 page read；
- `_read_oob` 同时请求 data 和 OOB 时，分别执行 page read 和 OOB read；
- `_write`：执行 page program，再执行 metadata OOB program；
- `_write_oob` 同时带 data 和 OOB 时，先 program page，再 program OOB；
- `_write_oob` 只有 OOB 时，只执行 OOB program；
- `_block_markbad` 只执行 OOB program。

任一步失败时按已经完成的字节准确设置 `retlen/oobretlen`。main 成功而 OOB
失败时返回错误，不伪报整页成功。

## 10. 错误与幂等语义

- 非 page-aligned `ADDR`：`STATUS.ERROR`；
- `OOB_LEN != 128`：`STATUS.ERROR`；
- OOB PROGRAM 的 PIO 数据少于 128 B：`STATUS.ERROR`；
- 目标块在操作前已经 bad：普通 program/erase 返回错误；
- 重复 `_block_markbad`：返回成功；
- 重复 OOB PROGRAM：按 NAND `old & incoming` 合并；
- OOB-only read 不产生 ECC corrected/uncorrectable 结果；
- main read 的 ECC/bitflip 语义保持不变。

## 11. 测试设计

### 11.1 静态 ABI 测试

- QEMU 和 Linux 头文件均不包含 `Q3N_CMD_MARK_BAD_BLOCK`；
- QEMU controller、media 和驱动均不包含专用 mark-bad helper；
- 明确断言 physical OOB head offset 为 `0x4000`；
- 明确断言 physical page size 为 `0x4680`。

### 11.2 QEMU layout 单元测试

- 构造带哨兵值的 18048 B physical page；
- logical OOB[0] 只更新 physical offset `0x4000`；
- logical OOB[1..127] 只更新 `0x4601..0x467f`；
- `0x4001..0x4600` 的 LDPC 哨兵保持不变；
- main `0x0000..0x3fff` 保持不变。

### 11.3 控制器单元测试

- `READ_PAGE_OOB` 只产生 128 B PIO 数据；
- `PROGRAM_PAGE_OOB` 只消费 128 B；
- OOB PROGRAM 不重新生成 LDPC；
- OOB PROGRAM 不改变 main；
- page PROGRAM 生成 LDPC，但不改变 OOB head/tail。

### 11.4 驱动静态/KUnit 测试

- `_block_markbad` 构造 `ff...ff` logical OOB，并把 byte 0 改为 `00`；
- `_block_markbad` 调用 OOB program helper；
- 普通 main+metadata 写按照 page PROGRAM -> OOB PROGRAM 顺序执行；
- OOB-only 写不携带 main buffer。

### 11.5 Guest 验收

- 擦除块第一页 OOB[0] 初始为 `ff`；
- 调用 `MEMSETBADBLOCK` 后 OOB[0] 为 `00`；
- `_block_isbad` 返回 bad；
- main data 在 markbad 前后逐字节一致；
- 注入并保存的 LDPC/bitflip 状态不因 markbad 改变；
- 直接向第一页 OOB[0] 写非 `ff` 值同样标坏；
- 重启后 BBM 仍存在；
- 不存在专用 mark-bad 命令统计或路径。

## 12. 文档更新

更新：

- `qemu/README.md`；
- 顶层 `README.md` 中涉及 OOB 的说明；
- `docs/qemu-3dnand-register-reference.md`；
- 脚本中的 ABI 断言和验收输出。

文档必须明确：

- physical OOB head 从 16 KiB 地址开始；
- logical OOB 在物理 page 中分成 head/tail；
- OOB 命令只传输 128 B；
- LDPC 不是 guest-visible OOB；
- 坏块标记完全通过 OOB PROGRAM 完成。
