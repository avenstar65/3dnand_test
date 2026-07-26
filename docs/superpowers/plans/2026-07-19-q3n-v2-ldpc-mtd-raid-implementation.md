# Q3N v2 LDPC、MTD ECC 与串行 Page-RAID Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把当前串行异步 Page-RAID 工程升级为 Q3NMEDIA v2，在每个 18048 B 物理页中保存 16 KiB main、1664 B physical OOB、16×96 B 模拟 LDPC，并向 Linux MTD 提供 128 B logical OOB、bitflips、`-EUCLEAN`、`-EBADMSG` 和 RAID 兜底语义。

**Architecture:** `q3n-media` 持久化 main、raw physical OOB 和 bitflip overlay；`q3n-nand` 生成/验证模拟 LDPC，映射非连续 logical OOB，并通过 MMIO 锁存 ECC 结果；Linux driver 将所有物理读统一为带 ECC 结果的接口，把 data metadata 和 parity manifest 真正持久化到 logical OOB[1..127]，最后聚合 MTD 统计并在目标页不可纠时使用串行 Page-RAID 恢复。

**Tech Stack:** Linux 7.0.12 direct MTD callbacks、QEMU 11.0.2 block backend/GLib、Q3NMEDIA sparse image、PCI MMIO、CRC32、KUnit、POSIX shell、guest MTD/UBI smoke tests。

## Global Constraints

- 工作目录固定为 `/Users/yangyu/Documents/3dnand-page-raid-serial-worktree`。
- 当前分支为 `codex/page-raid-serial-async-priority`，计划基线包含提交 `a3d87cc`。
- 设计规格为 `docs/superpowers/specs/2026-07-19-q3n-v2-ldpc-mtd-raid-design.md`。
- 不再创建额外 worktree；当前工作树在计划生成时为 clean。
- 物理页固定 18048 B：main 16384 B + physical OOB 1664 B。
- physical OOB[0] 为 BBM；physical OOB[1..1536] 为 16×96 B LDPC；physical OOB[1537..1663] 为 metadata。
- logical OOB 固定 128 B：logical[0] 映射 BBM，logical[1..127] 映射 physical[1537..1663]。
- logical OOB[0] 可读写；第一页写成非 `0xff` 后 block 必须变为 bad。
- ECC 固定 1 KiB/step、16 steps/page、40 bit/step、96 B LDPC/step、threshold 40。
- Q3NMEDIA v1 明确拒绝加载，不实现迁移。
- 不修改通用 MTD core、UBI 或 UBIFS。
- RAID 恢复成功形成 `-EUCLEAN`；最终失败才增加 `mtd->ecc_stats.failed` 并形成 `-EBADMSG`。
- 所有代码按 TDD 顺序实现，每个任务完成后独立提交。

---

## File Structure

```text
qemu/include/hw/mtd/q3n-nand.h  # 物理/逻辑几何、ECC、fault MMIO ABI
qemu/include/hw/mtd/q3n-media.h # v2 raw page 与持久化 overlay API
qemu/hw/mtd/q3n-media.c         # v2 image、18048 B slot、BBM、overlay
qemu/hw/mtd/q3n-nand.c          # OOB映射、模拟LDPC、译码、ECC寄存器

linux/drivers/mtd/nand/raw/qemu_3dnand.h       # Linux侧完全一致的MMIO ABI
linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h  # ECC结果和metadata helper接口
linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c  # metadata偏移、CRC验证、恢复验证
linux/drivers/mtd/nand/raw/qemu_3dnand_main.c  # OOB callbacks、MTD统计、RAID组合
linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c # metadata/OOB/ECC组合纯逻辑测试

rootfs/profile.d/mtd.sh          # guest OOB/bitflip/RAID验证命令
scripts/q3n-serial-smoke.sh      # 串行RAID自动验收入口
scripts/q3n-persistence-smoke.sh # v2重启持久化验收入口
tests/test_scripts.sh            # 静态ABI和结构门禁
README.md
qemu/README.md
```

---

### Task 1: 固定物理页、logical OOB 与 ECC MMIO ABI

**Files:**
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand.h`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces: all geometry/register names consumed by every later task.

- [ ] **Step 1: 添加失败的双侧 ABI 门禁**

在 `tests/test_scripts.sh` 对两侧头文件循环检查：

```sh
for symbol in \
  Q3N_PHYSICAL_OOB_SIZE Q3N_LOGICAL_OOB_SIZE Q3N_BBM_OOB_OFFSET \
  Q3N_LDPC_OOB_OFFSET Q3N_LDPC_BYTES_PER_STEP Q3N_LDPC_STEPS \
  Q3N_METADATA_OOB_OFFSET Q3N_REG_ECC_GEOM0 Q3N_REG_ECC_GEOM1 \
  Q3N_REG_ECC_STATUS Q3N_REG_ECC_MAX_BITFLIPS \
  Q3N_REG_ECC_CORRECTED_BITS Q3N_REG_ECC_FAILED_STEP \
  Q3N_REG_FAULT_STEP Q3N_REG_FAULT_FIRST_BIT \
  Q3N_REG_FAULT_COUNT Q3N_REG_FAULT_REGION \
  Q3N_FAULT_INJECT_BITFLIPS; do
  assert_contains qemu/include/hw/mtd/q3n-nand.h "$symbol"
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand.h "$symbol"
done
```

- [ ] **Step 2: 运行确认失败**

Run: `./scripts/smoke-test.sh`

Expected: FAIL on `Q3N_PHYSICAL_OOB_SIZE`.

- [ ] **Step 3: 添加固定几何常量**

QEMU 头使用 `1U << n`，Linux 头使用 `BIT(n)`：

```c
#define Q3N_PAGE_SIZE                  (16 * 1024)
#define Q3N_PHYSICAL_OOB_SIZE          1664U
#define Q3N_LOGICAL_OOB_SIZE           128U
#define Q3N_BBM_OOB_OFFSET             0U
#define Q3N_LDPC_OOB_OFFSET            1U
#define Q3N_LDPC_BYTES_PER_STEP        96U
#define Q3N_LDPC_STEPS                 16U
#define Q3N_LDPC_TOTAL_BYTES           1536U
#define Q3N_METADATA_OOB_OFFSET        1537U
#define Q3N_ECC_STEP_SIZE              1024U
#define Q3N_ECC_STRENGTH               40U
```

添加 build-time relationships：main/step=16、`1+1536+127=1664`、logical OOB=1+127。

- [ ] **Step 4: 添加不冲突的寄存器和状态位**

保留现有 `0x0000..0x0084`，从 `0x0088` 开始：

```c
Q3N_REG_ECC_GEOM0          = 0x0088,
Q3N_REG_ECC_GEOM1          = 0x008c,
Q3N_REG_ECC_STATUS         = 0x0090,
Q3N_REG_ECC_MAX_BITFLIPS   = 0x0094,
Q3N_REG_ECC_CORRECTED_BITS = 0x0098,
Q3N_REG_ECC_FAILED_STEP    = 0x009c,
Q3N_REG_FAULT_STEP         = 0x00a0,
Q3N_REG_FAULT_FIRST_BIT    = 0x00a4,
Q3N_REG_FAULT_COUNT        = 0x00a8,
Q3N_REG_FAULT_REGION       = 0x00ac,
Q3N_REG_STAT_LDPC_CORRECTED = 0x00b0,
Q3N_REG_STAT_LDPC_UNCORRECTABLE = 0x00b4,
Q3N_REG_STAT_LDPC_FAILED_STEPS = 0x00b8,
```

定义 CLEAN/CORRECTED/UNCORRECTABLE、`Q3N_ECC_NO_FAILED_STEP`、MAIN/LDPC region、`Q3N_FAULT_INJECT_BITFLIPS` 和独立的 `Q3N_STATUS_ECC_UNCORRECTABLE`。

- [ ] **Step 5: 验证并提交**

```bash
./scripts/smoke-test.sh
git diff --check
git add qemu/include/hw/mtd/q3n-nand.h \
        linux/drivers/mtd/nand/raw/qemu_3dnand.h tests/test_scripts.sh
git commit -m "feat: define q3n physical oob and ecc ABI"
```

Expected: smoke PASS.

---

### Task 2: 将 Q3NMEDIA 升级为 v2 物理页和持久化 overlay

**Files:**
- Modify: `qemu/include/hw/mtd/q3n-media.h`
- Modify: `qemu/hw/mtd/q3n-media.c`
- Modify: `qemu/README.md`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 1 geometry.
- Produces: raw main+1664 B OOB reads/programs and persistent main/LDPC overlay APIs.

- [ ] **Step 1: 添加失败的 v2/layout 门禁**

```sh
assert_contains qemu/hw/mtd/q3n-media.c 'Q3N_MEDIA_VERSION[[:space:]]+2'
assert_contains qemu/hw/mtd/q3n-media.c 'physical_oob_size'
assert_contains qemu/hw/mtd/q3n-media.c 'overlay_slots_offset'
assert_contains qemu/hw/mtd/q3n-media.c 'q3n_media_inject_bitflips'
assert_contains qemu/hw/mtd/q3n-media.c 'Q3N_METADATA_OOB_OFFSET'
```

Run: `./scripts/smoke-test.sh`; expected FAIL on media version 2.

- [ ] **Step 2: 定义 v2 header 和严格匹配条件**

在 4096 B header 的 reserved 区加入并写入：

```c
uint32_t physical_oob_size;
uint32_t logical_oob_size;
uint32_t ldpc_bytes_per_step;
uint32_t ldpc_steps;
uint64_t overlay_slots_offset;
uint64_t overlay_stride;
uint64_t overlay_slots_length;
```

所有字段使用 little-endian。`q3n_media_header_matches()` 必须同时验证 version=2、全部 geometry、所有 offset/length 和最终 image size；v1 header 返回明确的 incompatible error。

- [ ] **Step 3: 计算 v2 稀疏镜像布局**

```c
m->page_stride = (uint64_t)m->page_size + Q3N_PHYSICAL_OOB_SIZE;
m->overlay_stride = Q3N_PAGE_SIZE + Q3N_LDPC_TOTAL_BYTES;
m->overlay_slots_offset = QEMU_ALIGN_UP(
    m->page_slots_offset + m->page_count * m->page_stride,
    Q3N_MEDIA_HEADER_SIZE);
m->overlay_slots_length = m->page_count * m->overlay_stride;
m->image_size = m->overlay_slots_offset + m->overlay_slots_length;
```

每次乘加前使用 QEMU overflow helper；超出 `INT64_MAX` 时 realize 失败。保持 sparse truncate，不写满 page/overlay slots。

一个overlay bitmap bit对应一个介质bit，因此16 KiB main需要16384 B bitmap，1536 B LDPC需要1536 B bitmap，固定stride为17920 B；不得再次对介质字节数除以8。

- [ ] **Step 4: 改造 raw page API**

```c
int q3n_media_read_page(Q3NMedia *m, uint32_t block, uint32_t page,
                        uint8_t *data, uint8_t *physical_oob,
                        uint8_t *main_overlay, uint8_t *ldpc_overlay);
int q3n_media_program_page(Q3NMedia *m, uint32_t block, uint32_t page,
                           const uint8_t *data,
                           const uint8_t *physical_oob);
```

present page 读取完整 main+1664 B physical OOB；erased page 合成全 `0xff`，但 page 0 的 physical OOB[0] 从 BBM 位置读取。program 一次写完整 18048 B，维持严格 page frontier 和 1→0/单次 program 规则。

- [ ] **Step 5: 修正所有 BBM offset**

`create`、`load`、`get_block_status`、`mark_bad` 都使用：

```c
q3n_media_slot_offset(m, block, 0) + m->page_size +
    Q3N_BBM_OOB_OFFSET
```

program 第一页完成后根据 `physical_oob[0] != 0xff` 更新内存 bad 状态。

- [ ] **Step 6: 实现持久化 overlay XOR 注入**

```c
int q3n_media_inject_bitflips(Q3NMedia *m, uint32_t block, uint32_t page,
                              uint32_t step, uint32_t region,
                              uint32_t first_bit, uint32_t count);
```

MAIN 每 step 8192 bit，LDPC 每 step 768 bit。读取对应 overlay slot、逐 bit XOR、写回并 flush；重复注入相同范围恢复。erase block 把该块全部 overlay slot 写零。参数验证使用 `count <= limit - first_bit`，拒绝溢出。

增加覆盖MAIN和LDPC的step 0、1、2、15边界行为测试或可重复的focused harness，确认每个合法step只修改17920 B slot内对应bitmap区域，step 15末bit不越界，重复注入恢复原值。

- [ ] **Step 7: 验证构建和提交**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
git diff --check
git add qemu/include/hw/mtd/q3n-media.h qemu/hw/mtd/q3n-media.c \
        qemu/README.md tests/test_scripts.sh
git commit -m "feat: persist q3n v2 physical oob and bitflip overlays"
```

Expected: smoke and QEMU build PASS.

---

### Task 3: Controller 映射 logical OOB 并模拟 LDPC 译码

**Files:**
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `qemu/README.md`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 2 raw media API.
- Produces: controller-visible 128 B OOB, generated LDPC, latched ECC result and fault MMIO.

- [ ] **Step 1: 添加失败的结构门禁**

```sh
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_logical_to_physical_oob'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_physical_to_logical_oob'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_generate_ldpc_step'
assert_contains qemu/hw/mtd/q3n-nand.c 'q3n_decode_ldpc'
assert_contains qemu/hw/mtd/q3n-nand.c 'ecc_max_bitflips'
```

Run: `./scripts/smoke-test.sh`; expected FAIL on OOB mapping helper.

- [ ] **Step 2: 实现双向 OOB 映射**

```c
static void q3n_physical_to_logical_oob(const uint8_t *physical,
                                         uint8_t *logical)
{
    logical[0] = physical[Q3N_BBM_OOB_OFFSET];
    memcpy(logical + 1, physical + Q3N_METADATA_OOB_OFFSET,
           Q3N_LOGICAL_OOB_SIZE - 1);
}

static void q3n_logical_to_physical_oob(const uint8_t *logical,
                                         uint8_t *physical)
{
    physical[Q3N_BBM_OOB_OFFSET] = logical[0];
    memcpy(physical + Q3N_METADATA_OOB_OFFSET, logical + 1,
           Q3N_LOGICAL_OOB_SIZE - 1);
}
```

调用前把 physical OOB 填充为 `0xff`；映射函数永不触碰 LDPC[1..1536]。

- [ ] **Step 3: program 时生成 16 份模拟 LDPC**

```c
static void q3n_generate_ldpc_step(uint64_t page_key, uint32_t step,
                                   const uint8_t *data, uint8_t *ldpc);
```

使用 QEMU 可用的 CRC32C/CRC32 helper，以 page key、step、profile version 和 1024 B data 为 seed，确定性扩展为96 B。PROGRAM_PAGE 使用logical OOB全`0xff`；PROGRAM_PAGE_OOB使用guest提供的128 B logical OOB；两者最终都写入完整raw physical OOB。

- [ ] **Step 4: read 时判定 ECC 并锁存结果**

定义 `Q3NEccResult`，逐 step popcount main/LDPC overlay。0、1..40、>40 按设计返回 CLEAN/CORRECTED/UNCORRECTABLE；模拟码与原始main不匹配也不可纠。可纠时 data window 返回原始main；READ_PAGE_OOB额外返回映射后的128 B logical OOB。

- [ ] **Step 5: 接入 fault 和统计 MMIO**

FAULT参数寄存器写入state；`FAULT_CTRL.INJECT_BITFLIPS`调用Task 2 media API。read命令开始清空本次ECC结果，结束后锁存status/max/total/failed_step。物理统计分别累计corrected bits、uncorrectable pages和failed steps。

- [ ] **Step 6: 验证并提交**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
git diff --check
git add qemu/hw/mtd/q3n-nand.c qemu/README.md tests/test_scripts.sh
git commit -m "feat: map q3n logical oob and simulate ldpc decoding"
```

Expected: smoke and QEMU build PASS.

---

### Task 4: 将 data metadata 与 parity manifest 真正持久化

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 3 PROGRAM/READ_PAGE_OOB.
- Produces: logical OOB[1] data metadata/parity manifest, verified replay and CRC inputs for RAID recovery.

- [ ] **Step 1: 先写 KUnit metadata offset/validation tests**

增加测试：BBM byte 保持 `0xff`；data meta 从 logical[1] encode/decode；parity manifest 从logical[1] encode/decode；损坏header/data/parity CRC被拒绝。调用接口固定为：

```c
int q3n_pack_data_oob(u8 *logical_oob, size_t oob_len,
                      const struct q3n_data_meta *meta);
int q3n_unpack_data_oob(const u8 *logical_oob, size_t oob_len,
                        struct q3n_data_meta *meta);
int q3n_pack_parity_oob(u8 *logical_oob, size_t oob_len,
                        const struct q3n_parity_manifest *manifest);
int q3n_unpack_parity_oob(const u8 *logical_oob, size_t oob_len,
                          struct q3n_parity_manifest *manifest);
```

Run KUnit/build or structural test; expected FAIL because helpers do not exist.

- [ ] **Step 2: 实现 pack/unpack helpers**

所有pack先将128 B logical OOB填`0xff`，明确保持`logical_oob[0]=0xff`，从offset 1复制packed结构。unpack验证长度、magic/version/header CRC。不得使用C结构直接覆盖BBM。

- [ ] **Step 3: 扩展物理 page OOB helpers**

在driver中增加：

```c
static int qemu_3dnand_read_phys_page_oob_locked(...,
        u8 *data, u8 *logical_oob, u32 op_class,
        struct q3n_ecc_result *ecc);
static int qemu_3dnand_program_phys_page_oob_locked(...,
        const u8 *data, const u8 *logical_oob, u32 op_class);
```

data page program构造`q3n_data_meta`，写slot/stripe_id/data_crc；parity program构造manifest，写7个data CRC和parity CRC。

- [ ] **Step 4: 后台 parity rebuild 读取并验证 data metadata**

每个D成员使用READ_PAGE_OOB，验证metadata stripe/slot/data CRC，填入`rebuild.data_crc[slot]`。任一metadata或CRC错误终止parity build，stripe不能进入PROTECTED。

- [ ] **Step 5: 重启 replay 读取 parity manifest**

frontier指出parity page存在时，读取其OOB并验证manifest、stripe ID、parity CRC；只有通过才恢复`parity_index.valid`和CRC数组。无效manifest把stripe记为failed/unprotected，不把物理存在等同于PROTECTED。

- [ ] **Step 6: 验证并提交**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-kernel.sh
git diff --check
git add linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
        linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c tests/test_scripts.sh
git commit -m "feat: persist q3n raid metadata in logical oob"
```

Expected: smoke and kernel build PASS.

---

### Task 5: 实现 MTD logical OOB 读写和 BBM 写入语义

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `tests/test_scripts.sh`
- Modify: `rootfs/profile.d/mtd.sh`

**Interfaces:**
- Consumes: Task 4 OOB helpers.
- Produces: `mtd->oobsize=128`, `_read_oob`, `_write_oob`, writable logical OOB[0].

- [ ] **Step 1: 添加失败的静态和guest命令门禁**

```sh
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_read_oob'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_mtd_write_oob'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'Q3N_LOGICAL_OOB_SIZE'
assert_contains rootfs/profile.d/mtd.sh 'q3n-oob-bbm-test'
```

Run smoke; expected FAIL on `_read_oob` helper.

- [ ] **Step 2: 设置 logical OOB geometry 和 callbacks**

```c
mtd->oobsize = Q3N_LOGICAL_OOB_SIZE;
mtd->_read_oob = qemu_3dnand_mtd_read_oob;
mtd->_write_oob = qemu_3dnand_mtd_write_oob;
```

支持`MTD_OPS_PLACE_OOB`和`MTD_OPS_RAW`的精确logical offset/length；不把OOB[0]标成reserved。越页操作逐页推进并正确维护`retlen/oobretlen`。

- [ ] **Step 3: 实现 OOB-only program 语义**

OOB-only写入使用main全`0xff`与128 B logical OOB执行一次PROGRAM_PAGE_OOB，仍受物理页frontier和单次program约束。main+OOB同时提供时在一次program完成。已program页上的第二次OOB写返回`-EIO`，不模拟partial-page program。

如果OOB-only/raw写导致stripe中的data metadata不能通过后台校验，parity worker不得让frontier停在隐藏P页。它必须对P执行一次PROGRAM_PAGE_OOB：main全`0xff`，logical OOB[0]保持`0xff`，logical OOB[1..]写入独立的UNPROTECTED tombstone（包含magic/version/stripe ID/reason/header CRC）。成功后物理页为present且frontier推进；该页不增加protected计数，不可用于RAID恢复。replay识别tombstone后恢复UNPROTECTED状态并继续，不把它当作有效manifest。

- [ ] **Step 4: 连接 BBM 和 block status**

第一页OOB[0]写为非`0xff`后，QEMU media立即把block bad状态设为true；驱动下一次GET_BLOCK_STATUS、`_block_isbad`或重启probe都看到bad。`_block_markbad`继续写同一个物理字节，不能有第二份旁路标记。

- [ ] **Step 5: guest 验证并提交**

增加命令验证：读新块page0 OOB[0]=ff；顺序program page0并将logical OOB[0]=00；读回00；`mtd_is_bad`为true；physical LDPC区不可通过OOB长度访问。另验证offset=128/length>128拒绝、跨页OOB、PLACE/RAW、main+OOB单次program、good block已program页二次OOB写拒绝，以及OOB-only metadata无效stripe在P页写tombstone后下一stripe D0可继续program。

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-kernel.sh
git add linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
        rootfs/profile.d/mtd.sh tests/test_scripts.sh
git commit -m "feat: expose writable q3n logical oob and bbm"
```

Expected: build and smoke PASS.

---

### Task 6: 在 Linux 传播 ECC bitflips 和标准 MTD 统计

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 3 ECC result registers.
- Produces: common `q3n_ecc_result`, MTD geometry/stats, max-bitflip aggregation.

- [ ] **Step 1: 添加 ECC aggregation KUnit tests**

测试clean、两个页max(30,20)=30/total=50、threshold40、最终failure只计一次。定义纯helper：

```c
void q3n_ecc_accumulate(struct q3n_ecc_result *total,
                        const struct q3n_ecc_result *page);
```

Run kernel tests/build; expected FAIL because helper is missing.

- [ ] **Step 2: 定义统一物理读取结果**

```c
struct q3n_ecc_result {
	u32 max_bitflips;
	u32 corrected_bits;
	u32 failed_step;
	bool uncorrectable;
};
```

`qemu_3dnand_wait_ready()`只处理timeout和普通ERROR；read helper在命令结束后读取ECC寄存器。不可纠不立即转成`-EIO`。

- [ ] **Step 3: probe 验证并设置 MTD ECC fields**

读取GEOM0/1，严格匹配1024/40/96/16，否则probe返回`-EINVAL`。设置`ecc_step_size`、`ecc_strength`、`bitflip_threshold=40`。

- [ ] **Step 4: 前台读聚合和统计**

目标页可纠时`mtd->ecc_stats.corrected += corrected_bits`并聚合max；多页成功返回最大正bitflips，由MTD core形成threshold语义。目标不可纠先进入Task 7 RAID路径，不提前增加failed。OOB读取使用同一ECC语义。

- [ ] **Step 5: 区分后台统计**

parity build/replay读取不修改某次前台MTD stats，只增加`background_ecc_corrected_bits`；RAID recovery来源读取属于前台恢复，实际corrected bits计入MTD和`raid_source_corrected_bits`。

- [ ] **Step 6: 验证并提交**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-kernel.sh
git diff --check
git add linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
        linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c tests/test_scripts.sh
git commit -m "feat: propagate q3n ldpc bitflips through mtd"
```

Expected: smoke and build PASS.

---

### Task 7: 组合 LDPC failure、串行 RAID 恢复和 CRC

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Tasks 4 and 6 persistent manifest/ECC result.
- Produces: verified reconstruction, EUCLEAN on recovery, EBADMSG only on final failure.

- [ ] **Step 1: 添加恢复CRC KUnit tests**

扩展`q3n_recover_page`测试：正确XOR+目标CRC通过；错误parity或member导致目标CRC失败。新增接口：

```c
int q3n_verify_recovered_page(const u8 *data, size_t len,
                              __le32 expected_crc);
```

Expected: new test FAIL before helper exists.

- [ ] **Step 2: 目标不可纠时读取持久化 manifest**

只有entry当前且stripe PROTECTED才尝试恢复。READ parity main+OOB，验证manifest header、stripe/member bitmap和parity CRC；失败返回`-EBADMSG`。

- [ ] **Step 3: 所有来源先过 LDPC 和 metadata CRC**

parity或任一非目标data成员uncorrectable表示同stripe第二个失败成员，立即失败。可纠来源计入前台corrected统计；data metadata的stripe/slot/CRC必须与manifest一致。

- [ ] **Step 4: 验证目标CRC并映射最终返回值**

XOR后调用`q3n_verify_recovered_page()`对比manifest中的missing slot CRC。成功时`raid_recovered++`，逻辑max bitflips至少提升到40，且不增加failed；失败时清除输出有效性，`raid_failed++`和`mtd->ecc_stats.failed++`各一次，返回`-EBADMSG`。

- [ ] **Step 5: 后台不可纠保持UNPROTECTED**

parity worker读取任一D成员uncorrectable时不得program有效parity或manifest；必须在隐藏P页写main全`0xff`加UNPROTECTED tombstone以推进frontier，增加failed_stripes/background failure统计，并保持UNPROTECTED/FAILED。不得把`-EBADMSG`误当普通调度重试，tombstone也不得进入RAID恢复路径。

- [ ] **Step 6: 验证并提交**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-kernel.sh
git diff --check
git add linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_raid.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c tests/test_scripts.sh
git commit -m "feat: recover q3n ldpc failures through verified raid"
```

Expected: smoke and build PASS.

---

### Task 8: Guest 边界矩阵、重启持久化和文档

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `scripts/q3n-serial-smoke.sh`
- Modify: `scripts/q3n-persistence-smoke.sh`
- Modify: `tests/test_scripts.sh`
- Modify: `README.md`
- Modify: `qemu/README.md`

**Interfaces:**
- Produces: deterministic injection command, health counters and full acceptance evidence.

- [ ] **Step 1: 增加 debugfs 原子注入接口**

格式固定为：

```text
<physical-main-byte-address> <step> <main|ldpc> <first-bit> <count>
```

驱动严格解析5个字段并写FAULT_ADDR/STEP/REGION/FIRST_BIT/COUNT，最后写FAULT_CTRL触发。非法输入不改变任何fault state。

- [ ] **Step 2: 扩展统计输出**

debugfs/guest `q3n-stats`加入：LDPC corrected bits、uncorrectable pages、failed steps、background corrected、RAID source corrected、标准MTD corrected/failed（内核有导出时）。

- [ ] **Step 3: 增加前台边界测试**

每个场景使用新erase/program stripe，精确验证：

```text
39 main                    -> correct data, no EUCLEAN
40 main                    -> correct data, EUCLEAN
step0 30 + step1 30        -> corrected total 60, max 30
25 main + 16 LDPC same step -> LDPC fail
41 target protected        -> RAID recovery + EUCLEAN
41 target unprotected      -> EBADMSG
41 target + 41 peer        -> EBADMSG
41 target + 41 parity      -> EBADMSG
```

成功和EUCLEAN读取都用`cmp`验证数据，失败场景验证`retlen`和统计精确增量。

- [ ] **Step 4: 增加 OOB/BBM 与后台测试**

验证128 B logical OOB mapping、OOB[0]坏块写入、metadata从offset1开始；后台parity build遇到不可纠D成员时不产生有效manifest，但P页必须写UNPROTECTED tombstone并允许下一stripe继续program。

- [ ] **Step 5: 增加 v2 重启持久化测试**

空image创建v2；注入bitflip并关机；重启后同一overlay仍生效且RAID恢复结果一致。构造/保留v1 fixture时启动必须失败并包含`incompatible`诊断。

- [ ] **Step 6: 运行完整验证**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-serial-smoke.sh
./scripts/q3n-persistence-smoke.sh
```

Expected: static gates, all builds, serial guest smoke and two-boot persistence smoke PASS.

- [ ] **Step 7: 文档和最终提交**

```bash
git diff --check
git status --short
git add linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
        rootfs/profile.d/mtd.sh scripts/q3n-serial-smoke.sh \
        scripts/q3n-persistence-smoke.sh tests/test_scripts.sh \
        README.md qemu/README.md
git commit -m "test: verify q3n v2 ldpc oob and raid semantics"
git status --short
```

Expected: final worktree clean.

---

## Plan Self-Review Result

- Spec coverage: v2 image、18048 B page、BBM可写logical OOB[0]、96 B/step LDPC、17920 B/page persistent overlay、MTD ECC语义、后台统计隔离、manifest持久化、CRC恢复和重启测试均有明确任务。
- Existing-code correction: 当前manifest仅有结构/KUnit而未接入主路径，Task 4明确完成真实OOB持久化，不再假设其已经存在。
- Placeholder scan: 每个错误路径、offset、接口、命令和期望结果均已确定，无延后实现项。
- Type consistency: QEMU内部使用`Q3NEccResult`；Linux跨文件统一使用`struct q3n_ecc_result`；所有ABI名称在Task 1定义后复用。
- Scope: 不修改MTD core/UBI/UBIFS，不实现真实LDPC、v1迁移、FTL、GC或多成员RAID恢复。
