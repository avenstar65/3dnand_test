# Q3N Fixed Four-Plane Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不改变现有 identity 和 Page RAID 介质路径的前提下，为 Q3N 增加固定同 die 四 plane 的 READ、PROGRAM、OOB、ERASE 和 full-group read-retry 模式，通过 `nand_scan_with_ids()` 向 NAND Core 注册 64 KiB page、4 KiB OOB、100 MiB eraseblock、416 block 的逻辑 MTD。

**Architecture:** Kconfig 在 identity、Page RAID、multi-plane 三种存储模式间做编译期互斥选择。YTMC 厂商表同时保存完整 8-byte ID、物理 NAND 描述和 die/plane topology；probe 根据构建模式构造设备私有 scan IDs。Linux 保留 `cmdfunc/waitfunc` 和 ECC page callbacks，通过稳定 `q3n_page_*` 边界路由到独立 multi-plane layout、logical ops 和 MMIO 层。QEMU 新 group engine 用一次命令验证并执行固定 plane0..3，锁存 done/fail mask 与每 plane ECC 结果，底层继续复用单 physical page/block media API。

**Tech Stack:** Linux 7.0.12 raw NAND/NAND Core、legacy `cmdfunc/waitfunc`、ECC page/OOB/raw callbacks、C11 host tests、QEMU 11.0.2 QOM/MMIO/BlockBackend、POSIX shell、mtd-utils、sparse persistent image。

## Global Constraints

- 工作区固定为 `/Users/yangyu/Documents/3dnand-nand-core-ecc-read-retry-worktree`，分支固定为 `codex/nand-core-ecc-read-retry`。
- 已批准设计是 `docs/superpowers/specs/2026-08-01-q3n-four-plane-mode-design.md`；实现若与本计划示例冲突，以该设计的外部行为和数值为准，并在继续前修订本计划。
- 不直接修改下载后的 Linux 源码；Linux 通用层改动只能保存在 `linux/patches/*.patch` 并由 `scripts/apply-linux-patches.sh` 应用。本功能预计不新增通用层 patch，只扩展现有精确几何 patch 的测试证据。
- 不实现私有 MTD `_read/_write/_erase/_block_isbad/_block_markbad`；继续由 `nand_scan_with_ids()`、NAND Core bad-block path 和 NAND Core RAM BBT 接管。
- 不实现 `exec_op`；保留 legacy `cmdfunc/waitfunc/read_byte/read_buf/write_buf/select_chip`。
- `linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.c` 和 `.h` 不修改；identity 与 Page RAID 继续使用旧单 physical page 命令和现有全 `0xff` 跳写。
- multi-plane 与 Page RAID 编译期互斥。multi-plane 固定同 die 的 plane0..3，不接受 plane mask、2-plane 子集、单页 fallback、跨 die interleave 或运行时降级。
- multi-plane PROGRAM 即使某个或全部 16 KiB slice 为全 `0xff`，也必须提交一次完整四 plane command；ERASE 也始终提交四个 block。
- QEMU 对合法 group command 必须执行完四个成员才 READY，只产生一次 completion IRQ；允许部分成功，不 rollback、不自动 markbad、不实现掉电原子性。
- READ retry 的每个 mode 都重读完整四 plane logical page；ECC uncorrectable 用 per-plane ECC status 表达，不进入普通 media `fail_mask`。
- 首页 BBM offsets 固定为 `0/1024/2048/3072`；读取时 logical byte 0 为四者 AND，写入前四者 AND 后复制四份。完整 4096-byte scratch 必须为 per-device allocation，不放 kernel stack。
- multi-plane 默认镜像固定为 `work/media/q3n-nand-multiplane.raw`；不得删除、迁移或覆盖旧 `work/media/q3n-nand.raw`。
- 每个任务必须先产生预期失败（RED），再实现最小功能并得到通过（GREEN），然后运行该任务列出的回归并独立提交。
- 阶段提交前至少执行 `git diff --check`；最终完成前执行 host smoke、五模式 Linux 构建、QEMU 构建、multi-plane guest 与双启动持久化验收。

---

## File Structure

```text
linux/drivers/mtd/nand/raw/Kconfig.qemu_3dnand
    identity / Page RAID / multi-plane 三模式 choice
linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand
    公共对象与 RAID/MP 模式对象的互斥组成
linux/drivers/mtd/nand/raw/ytmc_nand.c/.h
    完整 ID 白名单、物理 nand_flash_dev、2-die/4-plane topology
linux/drivers/mtd/nand/raw/qemu_3dnand_flash.c/.h
    ID 匹配与基于选中逻辑几何的私有 scan IDs
linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.c/.h
    纯 profile、logical page/block 到 die/plane/physical block 映射、BBM helper
linux/drivers/mtd/nand/raw/qemu_3dnand_hw_multiplane.c/.h
    五条 MP 命令的 MMIO/PIO、READY 后 mask/ECC 采集
linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane.c/.h
    固定四 plane logical READ/PROGRAM/OOB/ERASE 和结果汇总
linux/drivers/mtd/nand/raw/qemu_3dnand_page.c/.h
    稳定 page API、typed storage mode 路由、资源生命周期
linux/drivers/mtd/nand/raw/qemu_3dnand_ecc.c
    NAND Core ECC callbacks、模式化 OOB layout、ECC 统计
linux/drivers/mtd/nand/raw/qemu_3dnand_init.c
    probe、ID/topology/CAP 校验、scan ID、nand_scan_with_ids、明确日志
linux/drivers/mtd/nand/raw/qemu_3dnand_regs.h
    Linux 侧 MP capability/command/register ABI

qemu/include/hw/mtd/q3n-nand.h
    QEMU 公共 MP capability/command/register ABI
qemu/hw/mtd/q3n-multiplane.c/.h
    callback-driven group validator/executor、映射、mask 和 ECC 数组
qemu/hw/mtd/q3n-nand.c
    Q3NNandState、MMIO dispatch、旧/新 command entry、一次 READY/IRQ
qemu/hw/mtd/q3n-media.c/.h
    不变的单 physical page/block persistent media API

configs/linux/q3n-identity.fragment
configs/linux/q3n-page-raid-2.fragment
configs/linux/q3n-page-raid-4.fragment
configs/linux/q3n-page-raid-8.fragment
configs/linux/q3n-multiplane.fragment
    可复现的五个 kernel 构建配置
scripts/configure-kernel.sh
    `--q3n-mode` 选择上述 fragment，默认 identity
scripts/q3n-kernel-matrix.sh
    五模式隔离构建与符号契约验证
scripts/run-qemu.sh
    `--nand-mode`、mode/config guard 和默认镜像选择
scripts/q3n-multiplane-smoke.sh
scripts/q3n-multiplane-persistence-smoke.sh
    新镜像 guest 单次与双启动验收
rootfs/profile.d/mtd.sh
rootfs/init
    multi-plane geometry/I/O/OOB/BBT/persistence guest entry

tests/test_q3n_multiplane_config.sh
tests/test_q3n_multiplane_layout.c/.sh
tests/test_q3n_hw_multiplane.c/.sh
tests/test_q3n_qemu_multiplane.c/.sh
tests/test_q3n_qemu_multiplane_abi.sh
tests/test_q3n_multiplane.c/.sh
    新模式的 host 级单元/契约测试
tests/test_linux_patches.sh
    64 KiB/1600 ppb/416 blocks/104-byte BBT 精确几何证据
tests/test_scripts.sh
tests/test_q3n_nand_core_contract.sh
scripts/smoke-test.sh
    overlay、构建对象、NAND Core ownership 和总回归
docs/qemu-3dnand-register-reference.md
    最终实现后的寄存器参考
```

### Stable data ownership

```c
enum q3n_storage_mode {
	Q3N_MODE_IDENTITY,
	Q3N_MODE_PAGE_RAID,
	Q3N_MODE_MULTIPLANE,
};

struct q3n {
	/* existing controller/NAND fields */
	enum q3n_storage_mode storage_mode;
	struct q3n_flash_topology topology;
	struct q3n_geometry physical_geometry;
	struct q3n_geometry geometry;
	u64 logical_size;
	struct q3n_page_profile page_profile;          /* identity/RAID */
	struct q3n_multiplane_profile multiplane_profile;
	const struct q3n_page_ops *page_ops;
	u8 *multiplane_oob_scratch;                    /* exactly 4096 B in MP */
};
```

`q3n->geometry` 和 `q3n->logical_size` 是所有模式的公共逻辑视图；RAID 私有状态继续在 `page_profile`，multi-plane 私有映射状态只在 `multiplane_profile`。不得用 union 复用二者，以免模式判断错误时把一种 profile 当成另一种解释。

---

### Task 1: 建立三模式 Kconfig choice 和可复现配置选择

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/Kconfig.qemu_3dnand`
- Modify: `configs/linux/mtd.fragment`
- Create: `configs/linux/q3n-identity.fragment`
- Create: `configs/linux/q3n-page-raid-2.fragment`
- Create: `configs/linux/q3n-page-raid-4.fragment`
- Create: `configs/linux/q3n-page-raid-8.fragment`
- Create: `configs/linux/q3n-multiplane.fragment`
- Modify: `scripts/configure-kernel.sh`
- Create: `tests/test_q3n_multiplane_config.sh`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**
- Produces Kconfig: `CONFIG_MTD_NAND_QEMU_3DNAND_IDENTITY`、existing `...PAGE_RAID`、new `...MULTIPLANE`。
- Produces CLI: `scripts/configure-kernel.sh --q3n-mode identity|page-raid-2|page-raid-4|page-raid-8|multiplane`。
- Default: no option means `identity`。

- [ ] **Step 1: 写失败的配置行为测试**

`tests/test_q3n_multiplane_config.sh` 在临时目录准备 fake Linux tree、记录参数的
fake `make` 和 fake `scripts/kconfig/merge_config.sh`，然后实际运行
`configure-kernel.sh`。断言：

- 不传 mode 时 merge 收到 identity fragment；
- 五个 mode 分别选择唯一且正确的 fragment；
- 三个 RAID mode 生成的配置分别得到 ratio 2/4/8；
- identity/multi-plane 生成配置不含 RAID ratio；
- 未知 mode 返回非零并给出可操作错误；
- 第二次执行不会把另一个 mode 的选择残留进 `.config`。

测试观察脚本的参数、退出码和生成配置，不 `grep` 实现源码。把测试加入
`scripts/smoke-test.sh`。真实 Kconfig choice 语义由 Task 9 五模式构建验证。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_q3n_multiplane_config.sh
```

Expected: FAIL，因为 `configure-kernel.sh` 尚不接受 `--q3n-mode`，也没有五个
mode fragments。

- [ ] **Step 3: 实现 Kconfig choice**

核心结构固定为：

```text
choice
	prompt "QEMU 3D NAND storage mode"
	default MTD_NAND_QEMU_3DNAND_IDENTITY
	depends on MTD_NAND_QEMU_3DNAND

config MTD_NAND_QEMU_3DNAND_IDENTITY
	bool "One physical page per logical page"

config MTD_NAND_QEMU_3DNAND_PAGE_RAID
	bool "Driver-side Page RAID"

config MTD_NAND_QEMU_3DNAND_MULTIPLANE
	bool "Fixed four-plane groups"
endchoice
```

保留 Page RAID ratio 的原符号、range/default 和 power-of-two probe 校验说明，仅让其依赖 `...PAGE_RAID`。

- [ ] **Step 4: 实现 mode fragments 和 configure 参数**

`configs/linux/mtd.fragment` 只保留通用 MTD 与 Q3N module 开关。五个 mode fragment 明确写一个 `=y` 以及其他模式的 `# ... is not set`；RAID fragment 分别写 ratio 2/4/8。

`configure-kernel.sh` 在 merge 前解析 `--q3n-mode`，映射到固定 fragment 路径；usage 列出全部值，未知值调用 `die`。在日志中打印 `Q3N storage mode: $q3n_mode`。

- [ ] **Step 5: 运行 GREEN 与旧 RAID 配置回归**

Run:

```bash
sh tests/test_q3n_multiplane_config.sh
sh tests/test_q3n_page_raid_config.sh
sh scripts/smoke-test.sh
```

Expected: all exit 0；旧 Page RAID 2:1/4:1/8:1 object test 仍通过。

- [ ] **Step 6: 提交**

```bash
git add linux/drivers/mtd/nand/raw/Kconfig.qemu_3dnand \
        configs/linux scripts/configure-kernel.sh \
        tests/test_q3n_multiplane_config.sh scripts/smoke-test.sh
git commit -m "build: add Q3N storage mode choice"
```

---

### Task 2: 把 YTMC topology 放入独立厂商表并实现纯四 plane 映射

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/ytmc_nand.c`
- Modify: `linux/drivers/mtd/nand/raw/ytmc_nand.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_flash.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_flash.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_init.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.h`
- Modify: `tests/test_q3n_flash.c`
- Create: `tests/test_q3n_multiplane_layout.c`
- Create: `tests/test_q3n_multiplane_layout.sh`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**
- Produces: `struct q3n_flash_topology`、`struct q3n_flash_info`。
- Replaces lookup result with: `const struct q3n_flash_info *q3n_flash_info_for_id(const u8 *id, size_t len)`。
- Produces: `q3n_multiplane_layout_build()`、`q3n_multiplane_map_page()`、`q3n_multiplane_map_block()`。

- [ ] **Step 1: 写失败的 topology 和字面映射测试**

`test_q3n_flash.c` 对 ID `9c d7 98 a6 51 33 4e 44` 断言：

```text
dies=2 planes_per_die=4 blocks_per_plane=247
data_blocks_per_plane=208 pages_per_block=1600
```

错误长度、任一 byte 不匹配和 NULL 均返回 NULL。

`test_q3n_multiplane_layout.c` 至少断言：

```text
logical geometry: writesize=65536 oobsize=4096 ppb=1600 blocks=416
logical size=43620761600
LB0 blocks=0,247,494,741
LB1 blocks=988,1235,1482,1729
LB414 blocks=207,454,701,948
LB415 blocks=1195,1442,1689,1936
page 1599 -> die0/block0/page1599
page 1600 -> die1/block0/page0
page 665599 -> die1/block207/page1599
page 665600 -> -ERANGE
```

还要覆盖 zero geometry、dies != 2、planes != 4、data blocks > blocks、topology ppb 与 physical ppb 不同、乘法溢出。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_q3n_flash.sh
sh tests/test_q3n_multiplane_layout.sh
```

Expected: first test compile 因 `q3n_flash_info` 缺失失败；second test compile 因新 header/API 缺失失败。

- [ ] **Step 3: 定义厂商信息**

`ytmc_nand.h` 定义：

```c
struct q3n_flash_topology {
	u32 dies;
	u32 planes_per_die;
	u32 blocks_per_plane;
	u32 data_blocks_per_plane;
	u32 pages_per_block;
};

struct q3n_flash_info {
	struct nand_flash_dev nand;
	struct q3n_flash_topology topology;
};

const struct q3n_flash_info *ytmc_nand_flash_info(void);
```

Host-test 分支补齐 `u32`。`ytmc_nand.c` 的首个条目保留现有 `nand_flash_dev` 数值和完整 ID，并在相邻 topology 字段保存 `2/4/247/208/1600`；以 `nand.name == NULL` 作为 sentinel。

- [ ] **Step 4: 改为完整 ID 返回 flash info**

```c
const struct q3n_flash_info *q3n_flash_info_for_id(const u8 *id, size_t len)
{
	const struct q3n_flash_info *info = ytmc_nand_flash_info();

	if (!id || len != YTMC_Q3N_ID_LEN)
		return NULL;
	for (; info->nand.name; info++) {
		if (info->nand.id_len == len && !memcmp(id, info->nand.id, len))
			return info;
	}
	return NULL;
}
```

临时把 `qemu_3dnand_init.c` 的现有 identity/RAID probe 改为保存 `info`，并把 `&info->nand` 传给现有 geometry/scan-ID helper，确保本任务结束时旧模式仍可构建。

- [ ] **Step 5: 实现 MP profile 与映射**

Header 固定导出：

```c
#define Q3N_MP_PLANES 4U

struct q3n_multiplane_profile {
	u32 dies;
	u32 planes_per_group;
	u32 blocks_per_plane;
	u32 data_blocks_per_plane;
	u32 pages_per_block;
	struct q3n_geometry physical;
	struct q3n_geometry logical;
	u64 logical_size;
};

struct q3n_mp_addr {
	u32 die;
	u32 block_in_plane;
	u32 page_in_block;
};

int q3n_multiplane_layout_build(struct q3n_multiplane_profile *profile,
		const struct q3n_geometry *physical,
		const struct q3n_flash_topology *topology);
int q3n_multiplane_map_page(const struct q3n_multiplane_profile *profile,
		u32 logical_page, struct q3n_mp_addr *addr);
int q3n_multiplane_map_block(const struct q3n_multiplane_profile *profile,
		u32 logical_block, struct q3n_mp_addr *addr);
int q3n_multiplane_physical_block(const struct q3n_multiplane_profile *profile,
		const struct q3n_mp_addr *addr, u32 plane, u32 *physical_block);
```

实现使用 checked `u64` multiplication。映射严格使用除法/取模公式，不使用 shift/mask；`map_block()` 令 `page_in_block=0`，`physical_block()` 拒绝 plane >= 4 和 block >= 208。

- [ ] **Step 6: 运行 GREEN 和地址层回归**

Run:

```bash
sh tests/test_q3n_flash.sh
sh tests/test_q3n_multiplane_layout.sh
sh tests/test_q3n_layout.sh
sh tests/test_q3n_addr.sh
git diff --check
```

Expected: all exit 0。

- [ ] **Step 7: 提交**

```bash
git add linux/drivers/mtd/nand/raw/ytmc_nand.* \
        linux/drivers/mtd/nand/raw/qemu_3dnand_flash.* \
        linux/drivers/mtd/nand/raw/qemu_3dnand_init.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.* \
        tests/test_q3n_flash.c tests/test_q3n_multiplane_layout.* \
        scripts/smoke-test.sh
git commit -m "feat: add Q3N four-plane topology mapping"
```

---

### Task 3: 扩展 scan IDs 与 Linux 非二次幂 patch 证据

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_flash.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_flash.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_init.c`
- Modify: `tests/test_q3n_flash.c`
- Modify: `tests/test_linux_patches.sh`

**Interfaces:**
- Produces generic scan builder: `q3n_flash_build_scan_ids(scan_ids, info, logical_geometry)`。
- Verifies existing patch helpers for 64 KiB page、1600 ppb、100 MiB eraseblock、416 blocks、104-byte BBT。

- [ ] **Step 1: 先把 MP scan ID 与 exact-geometry literals 加入测试**

`test_q3n_flash.c` 从 Task 2 MP profile 构造 scan IDs，断言：

```text
pagesize=65536 oobsize=4096 erasesize=104857600 chipsize=41600 MiB
id_len=8, full ID unchanged, options still include NAND_NON_POWER_OF_2_GEOMETRY
second entry all zero
```

把 `tests/test_linux_patches.sh` 的 profile 增加 `last_block` 和 `bbt_bytes` 字段，消除当前 literal `1663/416`，再增加：

```c
{
	"multiplane", 65536, 1600, 104857600, 416, 104,
	43620761600ULL, 665600,
	1599, 104792064, 1600, 104857600,
	664000, 43515904000ULL,
	665599, 43620696064ULL,
	43620761599ULL, 104857601,
},
```

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_q3n_flash.sh
sh tests/test_linux_patches.sh
```

Expected: flash test 因 builder 仍接受 `q3n_page_profile` 失败；patch test 因 hard-coded last block/BBT 或缺少 MP profile 失败。

- [ ] **Step 3: 泛化 scan builder**

固定签名：

```c
int q3n_flash_build_scan_ids(struct nand_flash_dev scan_ids[2],
		const struct q3n_flash_info *info,
		const struct q3n_geometry *logical);
```

用 checked `u64` 计算：

```text
erasesize = writesize * pages_per_block
size      = erasesize * blocks
chipsize  = size / MiB
```

要求 size 能整除 MiB、OOB fits `u16`、erasesize/chipsize fits NAND ID fields。复制 `info->nand` 后只替换 page/OOB/erase/chipsize，保留完整 ID/ECC/options；清零 sentinel。

调整 `qemu_3dnand_init.c` 现有调用使用 `&q3n->geometry`，不改变当前 identity/RAID 行为。

- [ ] **Step 4: 证明现有 Linux patch 已覆盖 MP**

`test_linux_patches.sh` 必须继续从 tracked `linux/patches/0001...` 和 `0003...` 抽取真实 helper body 编译，不能复制算法。`check_profile()` 用 profile 自带 `last_block` 和 `bbt_bytes` 断言 `nand_eraseblock_to_page()`、`nand_page_to_eraseblock()` 和 BBT allocation。

本任务不得直接编辑下载后的 `work/linux/linux-*/drivers/mtd/nand/raw/nand_base.c` 或 `nand_bbt.c`，也不得为 MP 新增无必要 patch。

- [ ] **Step 5: 运行 GREEN**

Run:

```bash
sh tests/test_q3n_flash.sh
sh tests/test_linux_patches.sh
sh scripts/smoke-test.sh
git status --short work/linux 2>/dev/null || true
```

Expected: tests exit 0；repo tracked change 中没有 Linux vendor source 文件。

- [ ] **Step 6: 提交**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_flash.* \
        linux/drivers/mtd/nand/raw/qemu_3dnand_init.c \
        tests/test_q3n_flash.c tests/test_linux_patches.sh
git commit -m "test: cover multi-plane exact NAND geometry"
```

---

### Task 4: 实现 Linux multi-plane MMIO/PIO 硬件层

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_regs.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_hw.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_hw.h`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_hw_multiplane.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_hw_multiplane.h`
- Modify: `tests/test_q3n_hw.c`
- Create: `tests/test_q3n_hw_multiplane.c`
- Create: `tests/test_q3n_hw_multiplane.sh`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**
- Produces capability: `Q3N_CAP_MULTIPLANE BIT(4)`。
- Produces commands 9..13 and registers `0xc4..0xe8` exactly as the design.
- Produces `q3n_hw_read_capabilities()` and five `q3n_hw_mp_*()` functions。

- [ ] **Step 1: 写 fake-MMIO RED tests**

`tests/test_q3n_hw_multiplane.c` 的 fake MMIO 记录全部 register reads/writes 和 DATA words。测试精确断言：

- READ main writes `MP_DIE/MP_BLOCK/MP_PAGE/READ_FLAGS/LEN=65536/CMD=9`，READY 后先读 masks，再读 selector 0..3 对应四组 ECC，最后读取 65536 bytes plane0..3 slices。
- PROGRAM writes exactly 65536 bytes even when all input is `0xff`，then `CMD=10`。
- OOB commands transfer 4096 bytes and use command 11/12。
- ERASE uses command 13 and no DATA access。
- `done=0x0f,fail=0` succeeds；`done=0x0d,fail=0x02` returns `-EIO` but preserves both masks；invalid overlap/incomplete mask returns `-EPROTO`。
- READY timeout returns `-ETIMEDOUT`；prevalidation error with masks zero returns `-EINVAL` or `-EIO` according to status, never fabricates plane completion。
- ECC uncorrectable with global ECC bit but no ordinary ERROR returns transport success and preserves per-plane uncorrectable result。

扩展 `test_q3n_hw.c` 证明旧 wait path 仍把 ordinary ERROR 转成 `-EIO`，并证明 `q3n_hw_read_capabilities()` 只读 CAP。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_q3n_hw_multiplane.sh
```

Expected: compile 因 `qemu_3dnand_hw_multiplane.h` 和 MP register macros 缺失失败。

- [ ] **Step 3: 增加共享 low-level helper，不改变旧语义**

`qemu_3dnand_hw.h/.c` 导出供独立 HW 实现层复用的内部 API：

```c
u32 q3n_hw_reg_read(struct q3n *q3n, u32 reg);
void q3n_hw_reg_write(struct q3n *q3n, u32 reg, u32 value);
int q3n_hw_wait_ready_status(struct q3n *q3n, u32 *status);
void q3n_hw_read_window(struct q3n *q3n, void *buffer, size_t length);
void q3n_hw_write_window(struct q3n *q3n, const void *buffer, size_t length);
u32 q3n_hw_read_capabilities(struct q3n *q3n);
```

`wait_ready_status()` 只等 READY 并返回 raw status；原 `q3n_hw_wait_ready()` 包装它并保留 ordinary ERROR -> `-EIO`。现有 identity/Page RAID tests 必须不变通过。

- [ ] **Step 4: 定义 per-plane transport results**

```c
struct q3n_mp_plane_result {
	int status;
	struct q3n_ecc_result ecc;
};

struct q3n_mp_result {
	u8 done_mask;
	u8 fail_mask;
	struct q3n_mp_plane_result plane[Q3N_MP_PLANES];
};
```

五个接口固定为：

```c
int q3n_hw_mp_read_page(struct q3n *, const struct q3n_mp_addr *,
		void *data, bool raw, struct q3n_mp_result *);
int q3n_hw_mp_program_page(struct q3n *, const struct q3n_mp_addr *,
		const void *data, struct q3n_mp_result *);
int q3n_hw_mp_read_oob(struct q3n *, const struct q3n_mp_addr *,
		void *oob, struct q3n_mp_result *);
int q3n_hw_mp_program_oob(struct q3n *, const struct q3n_mp_addr *,
		const void *oob, struct q3n_mp_result *);
int q3n_hw_mp_erase_group(struct q3n *, const struct q3n_mp_addr *,
		struct q3n_mp_result *);
```

- [ ] **Step 5: 实现 detail-preserving wait**

每条命令先清零 caller result，写地址/长度/数据/CMD，调用 `wait_ready_status()`；READY 后无论 ordinary ERROR 是否置位都先读 `DONE_MASK/FAIL_MASK`。合法完成必须满足 union `0x0f` 且不重叠；合法 partial failure 返回 `-EIO` 且 result 保留。READ main 在 transport mask 合法后按 `MP_ECC_SELECT=0..3` 读四组 ECC，再读取 data window；failed read slice 由 QEMU 保证为 `0xff`。

- [ ] **Step 6: GREEN 与旧硬件层回归**

Run:

```bash
sh tests/test_q3n_hw_multiplane.sh
sh tests/test_q3n_hw.sh
sh scripts/smoke-test.sh
git diff --check
```

Expected: all exit 0。

- [ ] **Step 7: 提交**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_regs.h \
        linux/drivers/mtd/nand/raw/qemu_3dnand_hw.* \
        linux/drivers/mtd/nand/raw/qemu_3dnand_hw_multiplane.* \
        tests/test_q3n_hw.c tests/test_q3n_hw_multiplane.* \
        scripts/smoke-test.sh
git commit -m "feat: add Q3N multi-plane MMIO layer"
```

---

### Task 5: 实现可独立测试的 QEMU 四 plane group engine

**Files:**
- Create: `qemu/hw/mtd/q3n-multiplane.c`
- Create: `qemu/hw/mtd/q3n-multiplane.h`
- Create: `tests/test_q3n_qemu_multiplane.c`
- Create: `tests/test_q3n_qemu_multiplane.sh`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**
- Produces callback-driven `q3n_multiplane_execute_read/program/read_oob/program_oob/erase()`。
- Produces QEMU-side `Q3NMultiplaneRequest`、`Q3NMultiplaneResult` 和 four-plane ops table。

- [ ] **Step 1: 写 group engine RED test**

Fake media callback 记录调用次序和 physical blocks，并可对指定 plane 返回 media failure 或 ECC uncorrectable。测试：

- die0/block0 -> blocks `0,247,494,741`；die1/block207 -> `1195,1442,1689,1936`。
- 五种 operation 都恰好按 plane0,1,2,3 调四次；某 plane 失败后仍继续后续 plane。
- successes 进入 done mask，media failures 进入 fail mask；最终 masks union `0x0f` 且互斥。
- read failure slice 在 callback 前/后保持全 `0xff`，成功 slice 位于 `plane * 16384`。
- OOB slice 位于 `plane * 1024`。
- ECC uncorrectable 仍是 done bit，不是 fail bit；四组 ECC 保留。
- invalid die/block/page/length 在任何 callback 前失败，masks 都为 0。
- raw read 透传 raw flag，ECC result 归零。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_q3n_qemu_multiplane.sh
```

Expected: compile 因 `q3n-multiplane.h` 缺失失败。

- [ ] **Step 3: 定义无 QOM 依赖的 engine API**

Header 只依赖标准/QEMU 基础整数类型，不暴露 `Q3NNandState`：

```c
typedef struct Q3NMultiplaneRequest {
	uint32_t die;
	uint32_t block;
	uint32_t page;
	uint32_t main_len;
	uint32_t oob_len;
	bool raw;
} Q3NMultiplaneRequest;

typedef struct Q3NMultiplaneEccResult {
	uint32_t status;
	uint32_t max_bitflips;
	uint32_t corrected_bits;
	uint32_t failed_step;
} Q3NMultiplaneEccResult;

typedef struct Q3NMultiplaneResult {
	uint8_t done_mask;
	uint8_t fail_mask;
	Q3NMultiplaneEccResult ecc[Q3N_MULTIPLANE_WIDTH];
} Q3NMultiplaneResult;
```

该独立 header 定义 `Q3N_MULTIPLANE_WIDTH=4`、`Q3N_MULTIPLANE_DIES=2`、
`Q3N_MULTIPLANE_BLOCKS_PER_PLANE=247`、
`Q3N_MULTIPLANE_DATA_BLOCKS_PER_PLANE=208`、
`Q3N_MULTIPLANE_PAGES_PER_BLOCK=1600`、
`Q3N_MULTIPLANE_MAIN_SLICE_SIZE=16384` 和
`Q3N_MULTIPLANE_OOB_SLICE_SIZE=1024`，
不包含 QOM header。`q3n-nand.c` 用 compile-time assertions 证明这些常量与
`q3n-nand.h` 的公共器件常量一致。Ops callbacks全部接收
`opaque, physical_block, page`；READ 额外接收 destination、raw 和 ECC out。
返回 0 表示 media success，负值表示该 plane media failure。

- [ ] **Step 4: 实现一次预校验和固定四成员循环**

地址验证只允许 die `<2`、block `<208`、page `<1600`；main 命令要求 65536，OOB 命令要求 4096，erase 不接受 transfer length。物理 block 公式固定：

```c
physical_block = (request->die * Q3N_PLANES_PER_DIE + plane) *
	Q3N_BLOCKS_PER_PLANE + request->block;
```

READ 先 `memset(buffer, 0xff, transfer_size)`；每次 callback 后记录 done/fail。PROGRAM/ERASE 即使前一 member 失败也继续。Engine 不发 IRQ、不设置全局 status，这些属于 `q3n-nand.c` integration。

- [ ] **Step 5: GREEN**

Run:

```bash
sh tests/test_q3n_qemu_multiplane.sh
sh tests/test_q3n_controller.sh
sh tests/test_q3n_overlay.sh
git diff --check
```

Expected: all exit 0。

- [ ] **Step 6: 提交**

```bash
git add qemu/hw/mtd/q3n-multiplane.* \
        tests/test_q3n_qemu_multiplane.* scripts/smoke-test.sh
git commit -m "feat: add QEMU Q3N four-plane engine"
```

---

### Task 6: 把 group engine 接入 QEMU MMIO ABI

**Files:**
- Modify: `qemu/include/hw/mtd/q3n-nand.h`
- Modify: `qemu/hw/mtd/q3n-nand.c`
- Modify: `qemu/hw/mtd/meson.build`
- Create: `tests/test_q3n_qemu_multiplane_abi.sh`
- Modify: `tests/test_q3n_controller.c`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**
- QEMU exposes `Q3N_CAP_MULTIPLANE`、commands 9..13、registers `0xc4..0xe8`。
- Q3NNandState stores MP address、done/fail、ECC selector/array；reset clears all MP state。

- [ ] **Step 1: 写 Linux/QEMU ABI 编译与设备行为 RED test**

`test_q3n_qemu_multiplane_abi.sh` 为 QEMU public header 创建最小 stub includes，
分别编译 Linux regs header 和 QEMU header 的小程序；两个程序输出 capability、
五个 command 和十个 register 的数值，测试比较字面输出完全相同。该测试验证
实际 C 定义可编译且 ABI 一致，不解析或 `grep` 源码。

DATA slice、mask/ECC selector、reset state 和五条 command 的行为继续由
`test_q3n_qemu_multiplane.c` 的真实 engine 调用与 `test_q3n_hw_multiplane.c`
的 fake-MMIO 交叉验证；meson 集成由本任务末尾真实 `build-qemu.sh` 验证。
把 ABI test 加入 smoke。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_q3n_qemu_multiplane_abi.sh
```

Expected: FAIL on missing QEMU capability/registers/commands。

- [ ] **Step 3: 扩展 Q3NNandState 和 staging window**

增加：

```c
uint32_t mp_die, mp_block, mp_page;
uint32_t mp_done_mask, mp_fail_mask, mp_ecc_select;
Q3NMultiplaneEccResult mp_ecc[Q3N_MULTIPLANE_WIDTH];
uint8_t data_buf[Q3N_PAGE_SIZE * Q3N_PLANES_PER_DIE];
```

DATA MMIO valid range同步扩到 65536。Main 与 OOB 命令复用该 buffer，不把 OOB 附加在 main 后面。写 `LEN`/`OOB_LEN` 继续重置 staging cursor。

- [ ] **Step 4: 为现有 single-page helpers 建 adapter**

把 `q3n_read_page()` 改为通过 out parameter 返回 `Q3NEccResult`；旧 READ command 把它复制到现有 scalar ECC registers。MP read adapter 转成 `Q3NMultiplaneEccResult`。

PROGRAM fault address必须从 adapter 当前 `(physical_block,page)` 计算，不能依赖旧 `s->addr`，从而可确定性让 MP 中一个 plane 失败。Program/OOB/erase adapters 继续调用现有 `q3n_media_*`，不复制 media 实现。

- [ ] **Step 5: 实现五条 command handlers**

每条 handler：

1. 清本次 done/fail；MP READ 还清四组 ECC。
2. 构造 request，并让 engine 完成一次性预校验和四 member 执行。
3. 复制 result masks/ECC 到 state。
4. 若 prevalidation 失败，masks 保持 0，调用一次 `q3n_finish_error()`。
5. 若 fail mask 非 0，调用一次 `q3n_finish_error()`；否则调用一次 `q3n_finish_ok()`。
6. READ 只在任一 per-plane ECC uncorrectable 时设置 global ECC bit，不设置 ordinary ERROR。

OOB/PROGRAM/ERASE 不改上次锁存的 MP ECC array；RESET 清地址、masks、selector、ECC array、cursor 和 retry mode。

- [ ] **Step 6: 更新 MMIO read/write 和 capability**

`MP_ECC_SELECT` 只接受 0..3；非法 write 设置 ERROR 且不改变 selector。ECC status/max/corrected/failed-step 从 selected entry 返回。`Q3N_REG_CAP` 返回原四能力加 `Q3N_CAP_MULTIPLANE`。每个 group command仍只由 handler 末尾发一次 READY/IRQ。

- [ ] **Step 7: GREEN、QEMU host 回归和真实构建**

Run:

```bash
sh tests/test_q3n_qemu_multiplane_abi.sh
sh tests/test_q3n_qemu_multiplane.sh
sh tests/test_q3n_controller.sh
sh tests/test_q3n_overlay.sh
./scripts/shell.sh ./scripts/build-qemu.sh
```

Expected: all exit 0；QEMU binary 生成；旧 command 0..8 tests 通过。

- [ ] **Step 8: 提交**

```bash
git add qemu/include/hw/mtd/q3n-nand.h qemu/hw/mtd/q3n-nand.c \
        qemu/hw/mtd/meson.build \
        tests/test_q3n_qemu_multiplane_abi.sh \
        tests/test_q3n_controller.c scripts/smoke-test.sh
git commit -m "feat: expose Q3N multi-plane MMIO commands"
```

---

### Task 7: 实现 Linux 固定四 plane logical page/OOB/erase 层

**Files:**
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane.c`
- Create: `linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_page.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_page.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Create: `tests/test_q3n_multiplane.c`
- Create: `tests/test_q3n_multiplane.sh`
- Modify: `tests/test_q3n_page.c`
- Modify: `tests/test_q3n_page.sh`
- Modify: `scripts/smoke-test.sh`

**Interfaces:**
- Produces `q3n_multiplane_page_ops` behind the unchanged stable `q3n_page_*` API。
- Changes init API to typed mode: `q3n_page_layer_init(q3n, mode, raid_data_pages, topology)`。
- Adds `failed_plane_mask` to `struct q3n_page_result`。

- [ ] **Step 1: 写 logical behavior RED tests**

Fake `q3n_hw_mp_*` 记录 logical mapping和调用数。覆盖：

- READ page 0/1600/665599 映射到正确 die/block/page；main 与可选 OOB各一次 group command。
- plane ECC max `{3,7,2,5}` -> logical max 7；corrected `{3,10,1,4}` -> sum 18。
- planes 1/3 uncorrectable -> `failed_plane_mask=0x0a`、`failed_data_pages=2`；普通 media fail mask -> `-EIO`。
- raw read仍完整四 plane，但 result 全零，不统计 ECC。
- PROGRAM main始终调用一次 MP command，包括 65536 bytes 全 `0xff`；main failure 后不发 OOB。
- main success + OOB request发两条命令，OOB failure 返回 `-EIO`，不 rollback。
- ERASE恰好一次 MP group；partial fail 返回 `-EIO`，不调用 markbad。
- 首页 OOB read 将 byte0设为四 BBM AND；其他三个原始 BBM位置仍保留。
- 首页 OOB write 在 scratch 中 AND offsets `0/1024/2048/3072` 并复制四份；caller buffer不被修改；非首页不归一化。

在 `test_q3n_page.sh` 增加第三个 `-DCONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE -DQ3N_TEST_MULTIPLANE` build，继续运行 identity 和 RAID build。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_q3n_multiplane.sh
sh tests/test_q3n_page.sh
```

Expected: compile 因 logical MP ops、typed mode 和 result field 缺失失败。

- [ ] **Step 3: 增加 BBM 纯 helpers**

在 MP layout 导出并单测：

```c
u8 q3n_multiplane_bbm_fold(const u8 oob[4096]);
void q3n_multiplane_bbm_normalize(u8 oob[4096]);
int q3n_multiplane_oob_free_region(unsigned int section,
		u32 *offset, u32 *length);
```

Free regions固定为 `(1,1023)/(1025,1023)/(2049,1023)/(3073,1023)`；section 4 返回 `-ERANGE`。

- [ ] **Step 4: 实现 logical ops**

`qemu_3dnand_multiplane.h` 只导出：

```c
const struct q3n_page_ops *q3n_multiplane_get_ops(void);
```

实现内部 map -> `q3n_hw_mp_*`。READ aggregate 使用 max/sum/popcount；transport error优先返回。PROGRAM不调用 `q3n_page_buffer_erased()`。首页 OOB normalization 使用 `q3n->multiplane_oob_scratch`，在已持 `q3n->lock` 的 page callback上下文内复制和修改。READ 首页只覆写 logical offset 0 为 folded value。

- [ ] **Step 5: 将 page layer 改为 typed mode**

```c
int q3n_page_layer_init(struct q3n *q3n,
		enum q3n_storage_mode mode, u32 raid_data_pages,
		const struct q3n_flash_topology *topology);
```

- identity: 原 `q3n_layout_build(...false...)` 和旧 ops。
- RAID: 原 `q3n_layout_build(...true...)`、原 scratch、原 `q3n_page_raid_get_ops()`。
- MP: `q3n_multiplane_layout_build()`，设置公共 geometry/size，分配 exactly 4096-byte OOB scratch，选择 MP ops。
- cleanup 按实际非 NULL pointer 释放两个 scratch；失败路径清 ops/mode，不能泄漏。

`qemu_3dnand_page_raid.c/.h` 不改。

`enum q3n_storage_mode` 明确定义在 `qemu_3dnand_priv.h`，该文件同时包含
`ytmc_nand.h` 和 `qemu_3dnand_multiplane_layout.h`，因此 `struct q3n` 中的
topology/profile 都是完整类型；不得把 enum 重复定义在 Kconfig 专用或测试专用
header 中。

- [ ] **Step 6: GREEN 和 Page RAID 文件不变证明**

Run:

```bash
sh tests/test_q3n_multiplane.sh
sh tests/test_q3n_page.sh
sh tests/test_q3n_layout.sh
git diff d6c2ff6 -- linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.h
```

Expected: tests exit 0；最后一个 diff无输出。

- [ ] **Step 7: 提交**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane.* \
        linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane_layout.* \
        linux/drivers/mtd/nand/raw/qemu_3dnand_page.* \
        linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
        tests/test_q3n_multiplane.* tests/test_q3n_page.* scripts/smoke-test.sh
git commit -m "feat: add Q3N fixed four-plane page operations"
```

---

### Task 8: 接入 NAND Core probe、ECC/OOB layout、read-retry 和构建对象

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_init.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_ecc.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_ecc.h`
- Modify: `tests/test_q3n_ecc.c`
- Modify: `tests/test_q3n_page_raid_config.sh`
- Modify: `tests/test_q3n_multiplane_config.sh`
- Modify: `tests/test_q3n_nand_core_contract.sh`

**Interfaces:**
- Probe requires full ID + topology + `Q3N_CAP_MULTIPLANE` in MP build。
- MP objects linked only under `CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE`。
- NAND Core sees MP OOB free sections and continues to own retry loop/BBT/bad-block interfaces。

- [ ] **Step 1: 扩展 object composition 和 compiled contract RED tests**

Makefile composition tests对三模式分别断言：

```text
identity: common only; no page_raid/multiplane objects
RAID: qemu_3dnand_page_raid.o only
MP: qemu_3dnand_multiplane_layout.o + qemu_3dnand_multiplane.o +
    qemu_3dnand_hw_multiplane.o; no page_raid.o
```

`test_q3n_nand_core_contract.sh` 在 MP config 下要求 MP ops/HW symbols存在、旧 legacy callbacks存在、`nand_scan_with_ids` 与 NAND Core bad-block/BBT ownership仍存在、没有 `exec_op` 和私有 MTD callbacks。Strings必须含明确 MP init log。

`test_q3n_ecc.c` 增加 `failed_plane_mask=0x0a/failed_data_pages=2` 的 account case，断言只增加一次 logical failed page，不按 plane 增加四次。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_q3n_page_raid_config.sh
sh tests/test_q3n_multiplane_config.sh
sh tests/test_q3n_ecc.sh
```

Expected: composition test FAIL，因为 Makefile 尚未链接 MP 对象。

- [ ] **Step 3: 实现模式对象互斥**

公共对象保持原列表；新增：

```make
qemu_3dnand-$(CONFIG_MTD_NAND_QEMU_3DNAND_PAGE_RAID) += \
		qemu_3dnand_page_raid.o
qemu_3dnand-$(CONFIG_MTD_NAND_QEMU_3DNAND_MULTIPLANE) += \
		qemu_3dnand_multiplane_layout.o \
		qemu_3dnand_hw_multiplane.o qemu_3dnand_multiplane.o
```

不得把 MP 对象放入 `qemu_3dnand-y`。

- [ ] **Step 4: 完成 probe mode selection 与 hard capability gate**

Compile-time helper只返回一种 `enum q3n_storage_mode`。Probe顺序固定：reset -> READ ID -> full whitelist match -> physical geometry/topology copy -> CAP read -> MP capability check -> page layer init -> generic scan IDs -> controller legacy init -> `nand_scan_with_ids()` -> register。

MP capability缺失返回 `-EOPNOTSUPP`，不得改选 identity；ID/topology无效返回 `-ENODEV/-EINVAL`。调用：

```c
ret = nand_scan_with_ids(&q3n->chip, 1, q3n->scan_ids);
```

不覆盖 `mtd->_read/_write/_erase/_block_isbad/_block_markbad`。

成功日志必须包含设计中完整字面值：

```text
mode=multiplane dies=2 planes/group=4
physical-page=16384 logical-page=65536 logical-oob=4096
pages/block=1600 logical-erasesize=104857600 logical-blocks=416
logical-size=43620761600 image-mode=multiplane
```

- [ ] **Step 5: 安装模式化 OOB layout**

identity/RAID继续使用现有 single-free-section ops。MP 使用独立 `mtd_ooblayout_ops`，其 free callback调用 Task 7 pure region helper；ECC region仍为空。`q3n_ecc_init()` 用公共 `q3n->logical_size` 校验 MTD，不再假设所有模式都用 RAID profile size。注册后断言 NAND Core计算的 `mtd->oobavail==4092`。

- [ ] **Step 6: 保持 NAND Core full-group read-retry**

`chip->read_retries=4` 和 `setup_read_retry=q3n_setup_read_retry` 不变。每次 `q3n_ecc_read_page()` 只调用一次 `q3n_page_read()`；在 MP mode这一次就是 full group。`q3n_setup_read_retry()` 继续写全局 retry register，因此下一次 MP READ 的四 plane共同使用该 mode。Raw callback不执行 ECC account。

- [ ] **Step 7: GREEN、overlay 后 module build**

Run:

```bash
sh scripts/smoke-test.sh
./scripts/shell.sh ./scripts/apply-linux-overlay.sh
./scripts/shell.sh ./scripts/configure-kernel.sh --q3n-mode multiplane
./scripts/shell.sh make -C /workspace/work/linux/linux-7.0.12 \
  O=/workspace/work/build/linux-7.0.12 ARCH=x86_64 \
  CROSS_COMPILE=x86_64-linux-gnu- drivers/mtd/nand/raw/
./scripts/shell.sh env Q3N_REQUIRE_KERNEL_BUILD=1 \
  Q3N_KERNEL_BUILD_DIR=/workspace/work/build/linux-7.0.12 \
  sh tests/test_q3n_nand_core_contract.sh
```

Expected: all exit 0；MP module imports NAND Core scan/registration and contains no private bad-block/MTD implementation。

- [ ] **Step 8: 提交**

```bash
git add linux/drivers/mtd/nand/raw/Makefile.qemu_3dnand \
        linux/drivers/mtd/nand/raw/qemu_3dnand_init.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_ecc.* \
        tests/test_q3n_ecc.c tests/test_q3n_page_raid_config.sh \
        tests/test_q3n_multiplane_config.sh \
        tests/test_q3n_nand_core_contract.sh
git commit -m "feat: register Q3N multi-plane NAND Core profile"
```

---

### Task 9: 更新 Linux/QEMU overlay 和五模式隔离构建矩阵

**Files:**
- Modify: `scripts/apply-linux-overlay.sh`
- Modify: `scripts/apply-qemu-overlay.sh`
- Modify: `scripts/shell.sh`
- Create: `scripts/q3n-kernel-matrix.sh`
- Modify: `tests/test_scripts.sh`
- Modify: `tests/test_q3n_nand_core_contract.sh`

**Interfaces:**
- Overlay copies all new Linux/QEMU source and header files idempotently。
- Matrix builds identity、RAID 2/4/8、multi-plane into separate build roots。

- [ ] **Step 1: 写 overlay/matrix RED 行为测试**

`tests/test_scripts.sh` 运行 overlay 脚本作用于 fake Linux/QEMU trees，断言六个
Linux files、QEMU `q3n-multiplane.c/.h` 和有效 meson entry 的实际输出状态；
第二次 apply 后 checksum 不变。

对 matrix，在临时 fixture repo 中复制待测 `q3n-kernel-matrix.sh` 和
`common.sh`，提供记录调用的 fake `uname`、`configure-kernel.sh`、`make` 和
compiled-contract script，实际运行 matrix。断言调用日志恰好包含五个 mode、
五个互不相同的 `BUILD_DIR=$work_dir/build-matrix/$mode`，且每个 build 后调用
一次 compiled contract。测试不检查脚本文本。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_scripts.sh
```

Expected: FAIL，因为 overlay未复制 MP files且 matrix script不存在。

- [ ] **Step 3: 更新 overlay**

Linux copy list增加：

```text
qemu_3dnand_multiplane_layout.c/.h
qemu_3dnand_hw_multiplane.c/.h
qemu_3dnand_multiplane.c/.h
```

QEMU overlay复制 `q3n-multiplane.c/.h` 到 `hw/block`，meson exact line为：

```text
system_ss.add(when: 'CONFIG_Q3N_NAND', if_true: files('q3n-media.c', 'q3n-multiplane.c', 'q3n-nand.c', 'q3n-pci.c'))
```

- [ ] **Step 4: 实现 matrix script**

macOS入口按现有模式 re-exec 到 `scripts/shell.sh`。Linux/container内循环：

```sh
for mode in identity page-raid-2 page-raid-4 page-raid-8 multiplane; do
	profile_build="$work_dir/build-matrix/$mode"
	BUILD_DIR="$profile_build" ./scripts/configure-kernel.sh --q3n-mode "$mode"
	BUILD_DIR="$profile_build" make -C "$linux_dir" \
		O="$profile_build/linux-$version" ARCH=x86_64 \
		CROSS_COMPILE=x86_64-linux-gnu- drivers/mtd/nand/raw/
	Q3N_KERNEL_BUILD_DIR="$profile_build/linux-$version" \
		Q3N_REQUIRE_KERNEL_BUILD=1 sh tests/test_q3n_nand_core_contract.sh
done
```

`scripts/shell.sh` 增加透传 `BUILD_DIR` 和本项目使用的 mode环境变量，但不得复用 `$HOME`。

- [ ] **Step 5: GREEN 静态验证**

Run:

```bash
sh tests/test_scripts.sh
sh scripts/smoke-test.sh
for script in scripts/*.sh; do sh -n "$script"; done
```

Expected: all exit 0。

- [ ] **Step 6: 执行真实五模式构建**

Run:

```bash
./scripts/q3n-kernel-matrix.sh
```

Expected: 五个 mode均打印 compiled NAND Core contract passed；identity无 RAID/MP symbols，RAID仅有 RAID symbol，MP仅有 MP symbols。

- [ ] **Step 7: 提交**

```bash
git add scripts/apply-linux-overlay.sh scripts/apply-qemu-overlay.sh \
        scripts/shell.sh scripts/q3n-kernel-matrix.sh \
        tests/test_scripts.sh tests/test_q3n_nand_core_contract.sh
git commit -m "build: verify all Q3N storage modes"
```

---

### Task 10: 增加显式镜像模式选择和 multi-plane guest 功能验收

**Files:**
- Modify: `scripts/run-qemu.sh`
- Create: `scripts/q3n-multiplane-smoke.sh`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `rootfs/init`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces CLI: `scripts/run-qemu.sh --nand-mode identity|page-raid|multiplane [--nand-image path] [--fresh-nand]`。
- Produces guest command: `mtd_q3n_multiplane_smoke` and `MTD_SMOKE=q3n-multiplane-smoke`。

- [ ] **Step 1: 写 runtime/guest RED 行为测试**

`tests/test_scripts.sh` 用 fake `uname`、kernel artifacts、`.config`、QEMU binary
和 image directory 实际运行 `run-qemu.sh`，从 fake QEMU 记录的 argv 断言：

- `--nand-mode multiplane` 选择 `q3n-nand-multiplane.raw`；
- identity/page-raid 选择旧 `q3n-nand.raw`；
- `--nand-image` 覆盖路径但不改变 mode；
- mode 与 `.config` 不匹配时 QEMU 未被调用且脚本失败；
- `--fresh-nand` 只重置解析后的一个 image。

`rootfs/profile.d/mtd.sh` 提供由 `rootfs/init` 调用的
`mtd_smoke_command_for()`；host test source 该文件并断言
`q3n-multiplane-smoke` 映射到 `mtd_q3n_multiplane_smoke`。功能覆盖不靠源码
marker，由本任务末尾真实 guest smoke 验证 first/last page、跨 eraseblock、
PLACE/RAW OOB、BBM 和 module reload。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_scripts.sh
```

Expected: FAIL on missing `--nand-mode` or guest entry。

- [ ] **Step 3: 实现 run-qemu mode/image guard**

默认 `--nand-mode identity`。映射：identity/page-raid -> `q3n-nand.raw`；multiplane -> `q3n-nand-multiplane.raw`。显式 `--nand-image` 只覆盖路径，不覆盖 mode。

启动前读取 kernel build `.config`，验证选中 mode与对应 Kconfig symbol为 `y`；不匹配时在 QEMU启动前失败，错误信息同时列出 requested mode 和 built symbol。`--fresh-nand` 只删除解析后的单个 explicit image path；不得删除两个默认镜像。

- [ ] **Step 4: 实现 guest geometry 和 I/O 验收**

`mtd_q3n_multiplane_smoke()` 动态查找 MTD 后先断言：

```text
writesize=65536 oobsize=4096 erasesize=104857600 size=43620761600
oobavail=4092 (从 sysfs 或 mtdinfo 可观察值)
```

然后按独立已擦除 block执行：

1. 写读 page 0，比较 65536-byte pattern。
2. 写读 page 1599，再擦下一个 logical block并写读 page 1600，验证 100 MiB边界。
3. 擦最后 block 415，写读 page 665599。
4. `mtd_badblock page-write/page-read` 分别验证 PLACE 和 RAW main+4096 OOB。
5. 首页 raw OOB向 offset 1024写 `0x00`，断言 logical offset 0及四个 BBM offsets读回 `00`，`MEMGETBADBLOCK`返回 bad。
6. 在另一个 block program first-page main，记录 digest，执行 `MEMSETBADBLOCK`，断言 main digest不变且四 BBM均为 00。
7. `modprobe -r qemu_3dnand; modprobe qemu_3dnand` 后重新查 MTD，断言 bad group仍被 NAND Core BBT观察。

函数失败路径只清临时文件，不修改旧 image。最终输出唯一 marker `q3n multi-plane smoke passed`。

- [ ] **Step 5: 实现 host wrapper 并构建 guest artifacts**

`scripts/q3n-multiplane-smoke.sh` 使用：

```sh
./scripts/run-qemu.sh --nand-mode multiplane --fresh-nand \
  --append "MTD_SMOKE=q3n-multiplane-smoke"
```

检查 guest pass marker和正常 poweroff marker。

- [ ] **Step 6: GREEN 静态/构建/guest 验收**

Run:

```bash
sh tests/test_scripts.sh
sh scripts/smoke-test.sh
./scripts/shell.sh ./scripts/configure-kernel.sh --q3n-mode multiplane
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-multiplane-smoke.sh
```

Expected: all exit 0；guest log含 geometry literals和 `q3n multi-plane smoke passed`。

- [ ] **Step 7: 证明旧镜像仍在且未被触碰**

Run:

```bash
test -e work/media/q3n-nand.raw
test -e work/media/q3n-nand-multiplane.raw
```

Expected: 两个文件都存在；本任务不要求两者内容兼容。

- [ ] **Step 8: 提交**

```bash
git add scripts/run-qemu.sh scripts/q3n-multiplane-smoke.sh \
        rootfs/profile.d/mtd.sh rootfs/init tests/test_scripts.sh
git commit -m "test: add Q3N multi-plane guest smoke"
```

---

### Task 11: 增加新镜像双启动持久化验收

**Files:**
- Create: `scripts/q3n-multiplane-persistence-smoke.sh`
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `rootfs/init`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces guest stages: `q3n-multiplane-persist-prepare` and `q3n-multiplane-persist-verify`。
- Uses only `work/media/q3n-nand-multiplane.raw`。

- [ ] **Step 1: 写 persistence entry RED 行为测试**

在临时 fixture repo 中运行真实 host persistence script，并用 fake
`run-qemu.sh` 记录参数、生成 sparse multi-plane image、分别输出固定 prepare/
verify digest与 BBM记录。断言两个 stage均传 `--nand-mode multiplane`、仅 prepare
传 `--fresh-nand`、相同 digest/BBM成功、任一 main/OOB/BBM不一致时脚本失败。

另外 source `rootfs/profile.d/mtd.sh` 并调用 `mtd_smoke_command_for()`，断言两个
persistence stage映射到各自 guest函数。测试运行产物，不检查源码文本。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
sh tests/test_scripts.sh
```

Expected: FAIL on missing persistence stages。

- [ ] **Step 3: 实现 prepare stage**

在 block 0写 main/OOB pattern；在 block 1写 main后 markbad。记录：

```text
q3n multi-plane persistence expected main_digest=<64 hex> oob_digest=<64 hex> bbm=00000000
```

`sync` 后正常 poweroff。OOB digest必须基于完整 raw 4096 bytes，不只检查 byte 0。

- [ ] **Step 4: 实现 verify stage**

第二次启动不 fresh，重新读相同 main/OOB并计算 digest；断言 block 1 bad，四 BBM为 00，bad first-page main仍与 prepare相同。输出：

```text
q3n multi-plane persistence verified main_digest=<64 hex> oob_digest=<64 hex> bbm=00000000
q3n multi-plane persistence verify passed
```

- [ ] **Step 5: 实现 host comparison 和 sparse image检查**

Host script逐字段比较 expected/verified，检查 image magic `Q3NMEDIA`，并用 logical bytes > allocated bytes证明 sparse。不得读取或重置 `q3n-nand.raw`。

- [ ] **Step 6: GREEN**

Run:

```bash
sh tests/test_scripts.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-multiplane-persistence-smoke.sh
```

Expected: 双启动 exit 0，digest/BBM相同，输出 `q3n multi-plane persistence verify passed`。

- [ ] **Step 7: 提交**

```bash
git add scripts/q3n-multiplane-persistence-smoke.sh \
        rootfs/profile.d/mtd.sh rootfs/init tests/test_scripts.sh
git commit -m "test: verify Q3N multi-plane persistence"
```

---

### Task 12: 同步寄存器文档并执行最终全矩阵验收

**Files:**
- Modify: `docs/qemu-3dnand-register-reference.md`
- Modify: `docs/superpowers/specs/2026-08-01-q3n-four-plane-mode-design.md` only if implementation names differ while behavior remains approved
- Modify: `README.md` if it contains the active run/build command index

**Interfaces:**
- Documentation must describe implemented code, not future design。
- Final evidence covers host、five Linux modes、QEMU、MP guest、MP persistence、old RAID regression。

- [ ] **Step 1: 建立实现证据清单**

人类阅读的寄存器文档不添加文本匹配测试。先保存以下已通过实现证据，作为
文档逐项核对输入：

```bash
sh tests/test_q3n_qemu_multiplane_abi.sh
sh tests/test_q3n_qemu_multiplane.sh
sh tests/test_q3n_hw_multiplane.sh
```

Expected: all exit 0，并输出已编译 ABI values、group mask/ECC behavior 和
Linux MMIO sequencing 的字面结果。Task 12 属于文档同步，不要求人为制造 RED。

- [ ] **Step 2: 更新寄存器与使用文档**

记录：CAP bit 4、commands 9..13、registers `0xc4..0xe8`、65536/4096 transfer、plane0..3 slice order、valid/prevalidation mask invariants、partial success、selected ECC、one READY/IRQ、RESET清理、非 timing-accurate限制。运行说明明确 `--nand-mode multiplane` 与新镜像，不声称真实并行性能或掉电原子性。

- [ ] **Step 3: 执行 host 全回归**

Run:

```bash
git diff --check
sh scripts/smoke-test.sh
```

Expected: exit 0；所有新增和旧 host tests通过。

- [ ] **Step 4: 执行 QEMU 与五模式 Linux 构建**

Run:

```bash
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/q3n-kernel-matrix.sh
```

Expected: QEMU build exit 0；五种独立 raw NAND build/compiled contract均通过。

- [ ] **Step 5: 执行 multi-plane guest 和 persistence**

Run:

```bash
./scripts/shell.sh ./scripts/configure-kernel.sh --q3n-mode multiplane
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-multiplane-smoke.sh
./scripts/q3n-multiplane-persistence-smoke.sh
```

Expected markers:

```text
q3n multi-plane smoke passed
q3n multi-plane persistence verify passed
```

- [ ] **Step 6: 执行旧模式回归且确认 Page RAID source 未变**

用现有 Page RAID 4:1构建启动旧镜像：

```bash
./scripts/shell.sh ./scripts/configure-kernel.sh --q3n-mode page-raid-4
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-serial-smoke.sh
git diff d6c2ff6 -- linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.c \
  linux/drivers/mtd/nand/raw/qemu_3dnand_page_raid.h
```

Expected: existing serial Page RAID smoke passes；diff无输出。若旧 `q3n-nand.raw` 已含测试坏块，使用显式备用旧格式 image path，不能删除用户原文件。

- [ ] **Step 7: 自审范围、占位符和类型一致性**

Run:

```bash
rg -n "TODO|TBD|FIXME|placeholder|not implemented" \
  linux/drivers/mtd/nand/raw/qemu_3dnand* qemu/hw/mtd/q3n-* \
  scripts/q3n-* rootfs/profile.d/mtd.sh
rg -n "exec_op|_block_isbad|_block_markbad|->_read|->_write|->_erase" \
  linux/drivers/mtd/nand/raw/qemu_3dnand*
rg -n "q3n_hw_(read_page|program_page|erase_block)" \
  linux/drivers/mtd/nand/raw/qemu_3dnand_multiplane.c
```

Expected: 第一条无未解释占位符；第二条无 driver-owned MTD/exec_op implementation（文档字符串除外）；第三条无输出，证明 MP 不走旧单页 fallback。逐一核对 Linux/QEMU 的 `u32/u64` transfer lengths、mask width、ECC fields 和 register values。

- [ ] **Step 8: 提交文档与最终测试调整**

```bash
git add docs/qemu-3dnand-register-reference.md \
        docs/superpowers/specs/2026-08-01-q3n-four-plane-mode-design.md \
        README.md
git commit -m "docs: document Q3N four-plane interface"
```

只 add 实际修改的文件；未修改的 design/README 不得为了凑命令制造空改动。

---

## Final Acceptance Checklist

- [ ] Kconfig 三模式互斥，默认 identity；RAID ratio 只在 RAID 可见。
- [ ] 完整 8-byte ID 匹配后从 `ytmc_nand.c` 获得 `2/4/247/208/1600` topology。
- [ ] MP probe在 `nand_scan_with_ids()` 前完成 capability、profile 和 private scan ID，并在 capability缺失时硬失败、绝不 fallback。
- [ ] NAND Core注册几何精确为 `65536/4096/104857600/43620761600/416`，OOB available为4092，RAM BBT为104 bytes。
- [ ] Linux MP READ/PROGRAM/OOB/ERASE只使用 commands 9..13，固定四 plane，不调用旧单页 HW接口。
- [ ] MP PROGRAM对全 `0xff` slice不跳写；identity/Page RAID的原全 `0xff`优化不变。
- [ ] QEMU合法命令 masks union为0x0f且互斥；预校验失败masks为0且介质不变；partial failure可观测且不rollback。
- [ ] ECC uncorrectable只进入per-plane ECC；每个 retry mode重读完整group，corrected=sum、max bitflips=max、failed plane mask准确。
- [ ] 首页四 BBM读AND、写归一化；任一 plane bad使logical group bad；markbad不改变main；NAND Core BBT重扫可见。
- [ ] `cmdfunc/waitfunc`、NAND Core bad-block/BBT和patch-only精确几何路径保持。
- [ ] QEMU一次group command只产生一次READY/completion IRQ；文档不声称真实并行性能、回滚或掉电原子性。
- [ ] identity、RAID 2:1/4:1/8:1、multi-plane五个隔离构建通过。
- [ ] QEMU build、host smoke、MP guest、MP双启动持久化和旧 Page RAID guest回归通过。
- [ ] `qemu_3dnand_page_raid.c/.h` 相对设计基线无改动。
- [ ] `q3n-nand.raw` 和 `q3n-nand-multiplane.raw` 同时保留，旧镜像未删除/迁移。
- [ ] `git diff --check`无输出，工作区仅含有意变更，所有阶段提交可独立审查。
