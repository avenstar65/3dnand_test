# Q3N 传统 cmdfunc/waitfunc 迁移实施计划

> **执行要求：** 按任务逐项使用测试先行（RED → GREEN → REFACTOR），实现完成前不得恢复或保留 `exec_op`。

**目标：** 在不改变现有 NAND Core ECC page/OOB、read retry、坏块表和非二次幂几何方案的前提下，把 Q3N 控制器从 `nand_controller_ops.exec_op` 迁移到传统 `cmdfunc + waitfunc` 回调。

**架构：** `nand_scan_with_ids()` 仍使用 `ytmc_nand.c` 提供的完整 8-byte ID 白名单和几何初始化 MTD。NAND Core 通过 legacy callbacks 完成 RESET、READID、STATUS、ERASE 命令控制；16 KiB page、1 KiB OOB 和 raw I/O 继续由 `chip->ecc.*` 回调直接访问 Q3N 硬件；read retry 继续由 `chip->ops.setup_read_retry` 驱动。寄存器访问仍只能存在于 `qemu_3dnand_hw.c`。

**技术基线：** Linux 7.0.12 Raw NAND、QEMU 11.0.2、C11 宿主行为测试、现有 Linux overlay/patch、QEMU guest smoke test。

## 全局约束

- 只在 `codex/nand-core-ecc-read-retry` 隔离分支中工作。
- 不直接修改 Linux 上游源码；非二次幂支持继续由 `linux/patches/` 注入。
- `q3n_controller_ops` 只能保留 `.attach_chip`，不得设置 `.exec_op`。
- 不改变 `chip->ecc.read_page/write_page/read_page_raw/write_page_raw/read_oob/write_oob` 的数据路径。
- 不由 Q3N 驱动直接设置任何 `mtd->_read/_write/_erase/_read_oob/_write_oob/_block_isbad/_block_markbad`。
- 不实现私有 BBT，不设置 `NAND_SKIP_BBTSCAN`。
- 本次不实现 Page RAID，不恢复旧 scheduler/RAID 对象。
- 只支持 chip select 0 和单 LUN。
- full ID 仍为 `9c d7 98 a6 51 33 4e 44`；前缀和任意字节变体必须拒绝。
- legacy pending error 在 `waitfunc()` 消费前不得被后续命令覆盖。
- 每个任务完成后运行聚焦测试；所有实现完成后再运行全量构建和 guest 验证。

---

## Task 1：建立 legacy 命令状态机的失败测试

**文件：**

- 新建：`tests/test_q3n_legacy.c`
- 新建：`tests/test_q3n_legacy.sh`
- 修改：`scripts/smoke-test.sh`
- 修改：`linux/drivers/mtd/nand/raw/qemu_3dnand_controller.h`
- 修改：`linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`

**待实现接口：**

```c
struct q3n_legacy_state {
	u8 data[YTMC_Q3N_ID_LEN];
	u8 data_len;
	u8 data_pos;
	u32 erase_page;
	int error;
	bool erase_pending;
};

void q3n_legacy_state_init(struct q3n *q3n);
void q3n_legacy_command(struct q3n *q3n, unsigned int command,
			int column, int page_addr);
u8 q3n_legacy_read_byte_value(struct q3n *q3n);
void q3n_legacy_read_buffer(struct q3n *q3n, u8 *buf, int len);
void q3n_legacy_write_buffer(struct q3n *q3n, const u8 *buf, int len);
int q3n_legacy_wait(struct q3n *q3n);
int q3n_legacy_select(struct q3n *q3n, int chipnr);
```

- [ ] 用链接时 fake `q3n_hw_*` 的宿主测试覆盖 RESET、READID、STATUS、ERASE1/ERASE2、read byte/buffer、write buffer、select chip。
- [ ] READID 测试要求一次硬件读取完整 8-byte ID，并验证连续读取 2 byte 后重新 READID 可读取完整 8 byte。
- [ ] ERASE 测试覆盖 page 0、1599（拒绝）、1600（接受）、最后一个 block 起始页和越界 page。
- [ ] 错误测试证明：首个错误不会被后续命令覆盖；错误由 `waitfunc` 返回一次后清除；错误期间不会新增硬件事务。
- [ ] RESET 测试证明成功后清除 staging、pending erase 和 retry mode；失败后 `read_byte` 返回 `0xff`。
- [ ] page/OOB sequencing 测试证明 READ0、READOOB、SEQIN、PAGEPROG 不触发硬件 page I/O。
- [ ] 运行 `./tests/test_q3n_legacy.sh`，确认因接口尚未实现而失败（RED）。
- [ ] 仅增加测试所需的结构和声明，不增加实现行为。
- [ ] 提交测试基线：`test: define Q3N legacy callback behavior`

## Task 2：实现 host-testable legacy 状态机

**文件：**

- 修改：`linux/drivers/mtd/nand/raw/qemu_3dnand_controller.c`
- 修改：`linux/drivers/mtd/nand/raw/qemu_3dnand_controller.h`
- 修改：`linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- 修改：`tests/test_q3n_legacy.c`

**命令矩阵：**

| 命令 | 状态机行为 |
| --- | --- |
| RESET | `q3n_hw_reset()`；成功后初始化 legacy state 并恢复 retry mode 0 |
| READID | 只接受 column 0；调用 `q3n_hw_read_id()` 并装载 8-byte staging |
| STATUS | 调用 `q3n_hw_read_status()` 并装载 1-byte staging |
| ERASE1 | 校验 row page 范围及 1600-page 对齐，只 latch row |
| ERASE2 | 必须跟随合法 ERASE1，换算 block 后调用 `q3n_hw_erase_block()` |
| READ0/READOOB/SEQIN/PAGEPROG | 接受 sequencing，不执行 page/OOB 硬件事务 |
| RNDOUT/RNDIN/其他 | 记录 `-EOPNOTSUPP` |

- [ ] 在 `Q3N_HOST_TEST` 下提供最小 NAND command 常量兼容层，使同一份生产状态机可直接被宿主测试编译。
- [ ] 删除现有 operation parser、instruction finder 和所有 `q3n_exec_*`/`q3n_exec_op` 实现。
- [ ] 实现 staging reset/load/read，确保 buffer 耗尽返回 `0xff`。
- [ ] 实现 first-error-wins helper；有 pending error 时拒绝后续硬件命令。
- [ ] 实现 ERASE1/ERASE2 两阶段状态和精确 1600-page/block 校验。
- [ ] `q3n_legacy_wait()` 优先返回并清除 pending errno，否则读取硬件 status。
- [ ] `q3n_legacy_write_buffer()` 明确记录 `-EOPNOTSUPP`，不访问数据窗口。
- [ ] 运行 `./tests/test_q3n_legacy.sh`，确认所有状态机测试通过（GREEN）。
- [ ] 使用边界变异检查错误覆盖、擦除除数、ID 长度和 status staging，确认测试能失败。
- [ ] 重构重复 staging/error 逻辑并再次运行测试（REFACTOR）。
- [ ] 提交：`mtd: add Q3N legacy NAND command state machine`

## Task 3：注册 Linux legacy callbacks 并移除 exec_op

**文件：**

- 修改：`linux/drivers/mtd/nand/raw/qemu_3dnand_controller.c`
- 修改：`linux/drivers/mtd/nand/raw/qemu_3dnand_controller.h`
- 修改：`linux/drivers/mtd/nand/raw/qemu_3dnand_init.c`
- 修改：`tests/test_q3n_nand_core_contract.sh`

**Linux 适配接口：**

```c
void q3n_controller_legacy_init(struct nand_chip *chip);

static void q3n_cmdfunc(struct nand_chip *chip, unsigned int command,
			int column, int page_addr);
static int q3n_waitfunc(struct nand_chip *chip);
static u8 q3n_read_byte(struct nand_chip *chip);
static void q3n_read_buf(struct nand_chip *chip, u8 *buf, int len);
static void q3n_write_buf(struct nand_chip *chip, const u8 *buf, int len);
static void q3n_select_chip(struct nand_chip *chip, int chipnr);
```

- [ ] 先扩展编译产物契约测试：要求模块包含 legacy 初始化/回调路径，且源码和符号中均不存在 `exec_op`/`q3n_exec_op`。
- [ ] 运行 `./tests/test_q3n_nand_core_contract.sh`，确认旧实现导致失败（RED）。
- [ ] 编写 `nand_chip` 薄适配回调，每次状态变化和单次 MMIO 事务使用 `q3n->lock`；不得跨 ERASE1/ERASE2 持锁。
- [ ] 在 probe 中先 `nand_controller_init()`、设置只含 `attach_chip` 的 controller ops，再调用 `q3n_controller_legacy_init()`，最后调用 `nand_scan_with_ids(chip, 1, ids)`。
- [ ] 保持 `ytmc_nand.c` ID/几何表、完整 ID 白名单匹配、ECC callbacks 和 setup_read_retry 注册不变。
- [ ] 保持 production KO 对象列表为现有 8 个，不新增额外生产对象。
- [ ] 应用 overlay 到干净 Linux 7.0.12 工作树并构建模块。
- [ ] 运行契约测试，确认 `nand_scan_with_ids`、NAND Core BBT/坏块符号仍存在，`exec_op` 已消失（GREEN）。
- [ ] 用 `nm` 和源文件扫描确认 Q3N 未实现直接 MTD 回调、私有 BBT 或直接寄存器访问。
- [ ] 提交：`mtd: switch Q3N controller to legacy callbacks`

## Task 4：验证 NAND Core ECC、read retry 和非二次幂路径未回归

**文件：**

- 视测试缺口修改：`tests/test_q3n_ecc.c`
- 视测试缺口修改：`tests/test_q3n_hw.c`
- 视测试缺口修改：`tests/test_linux_patches.sh`
- 修改：`docs/superpowers/specs/2026-07-29-q3n-legacy-cmdfunc-waitfunc-design.md`

- [ ] 运行 `./tests/test_q3n_ecc.sh`，验证 ECC page/raw/OOB callbacks 和 retry 0→1→2→3→0。
- [ ] 运行 `./tests/test_q3n_hw.sh`，验证 RESET、READID、STATUS、ERASE 寄存器事务。
- [ ] 运行 `./tests/test_linux_patches.sh`，验证三段 patch 可重复应用且 `nand_base.c`/`nand_bbt.c` 精确几何仍生效。
- [ ] 运行 `./scripts/smoke-test.sh`，验证所有宿主测试。
- [ ] 把设计文档状态改为“已实现”，记录最终回调、锁、错误传播和测试证据；不得把旧 `exec_op` 描述保留为当前架构。
- [ ] 更新原 NAND Core 总设计/实施文档中仍描述 `exec_op` 的当前态章节，历史方案要明确标注“已被 legacy callback 迁移替代”。
- [ ] 提交：`docs: record Q3N legacy callback implementation`

## Task 5：完成 Linux/QEMU/guest 端到端验证

**文件：**

- 必要时修改：`scripts/build-linux.sh`
- 必要时修改：`scripts/run-qemu-smoke.sh`
- 必要时修改：`README.md`
- 必要时修改：`qemu/README.md`

- [ ] 在干净 Linux 7.0.12 源码上按顺序应用 `0001`、`0002`、`0003` patch 和 Q3N overlay。
- [ ] 构建 Q3N `.ko` 和 guest 使用的 `bzImage`；保存最终成功命令与日志摘要。
- [ ] 构建或复用匹配的 QEMU 11.0.2 x86_64 binary，运行 Q3N guest smoke。
- [ ] guest 中确认 probe 成功、MTD 几何为 16 KiB page / 1 KiB OOB / 25 MiB eraseblock / 1664 blocks。
- [ ] 验证 erase、page read/write、OOB read/write、`MEMGETBADBLOCK`、`MEMSETBADBLOCK`、重新加载后的 RAM BBT 扫描。
- [ ] 验证 persistent backing 下写入、关机、重启和读回。
- [ ] 检查 guest dmesg 中没有 `exec_op` 相关错误、legacy callback 缺失、超时或 lock warning。
- [ ] 运行最终全量验证：宿主 smoke、Linux build、QEMU build/测试、guest smoke。
- [ ] 提交最终实现：`mtd: complete Q3N legacy NAND callback migration`

## Task 6：推送 GitHub 并更新现有 PR

- [ ] 检查 `git status`，确认没有生成文件、日志或无关修改进入提交。
- [ ] 检查从目标基线到 HEAD 的 diff，确认只包含已批准的 legacy 迁移、测试和文档。
- [ ] 推送 `codex/nand-core-ecc-read-retry` 到 `origin`。
- [ ] 更新现有 draft PR #2 描述：去掉 `exec_op` 当前态，加入 `cmdfunc/waitfunc`、测试证据和未实现 Page RAID 的范围说明。
- [ ] 确认远端 PR HEAD 与本地 HEAD 一致，记录 PR 链接。

## 完成标准

以下条件全部满足才可宣告完成：

1. 模块通过 `nand_scan_with_ids()` 和独立 `ytmc_nand.c` full-ID 白名单完成初始化。
2. `q3n_controller_ops` 不注册 `exec_op`，传统 callbacks 完整注册。
3. RESET、READID、STATUS、ERASE 使用 `cmdfunc/read_byte/waitfunc` 路径。
4. page/OOB/raw 仍走 `chip->ecc.*`，read retry 仍能完成 0→1→2→3 迭代并恢复 0。
5. MTD I/O、坏块接口和 RAM BBT 仍由 NAND Core 提供。
6. 非二次幂几何只通过版本化 Linux patch 修改上游层，未直接提交 Linux 源码改动。
7. 宿主测试、Linux/QEMU 构建和 guest smoke 均通过。
8. 文档、实现、测试和 GitHub PR 对当前架构的描述一致。
