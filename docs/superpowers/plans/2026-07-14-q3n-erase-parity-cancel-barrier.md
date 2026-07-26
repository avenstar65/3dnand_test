# QEMU 3D NAND Erase/Parity Cancel Barrier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为串行 `D0..D6,P` Page-RAID 增加按 block 隔离的 erase/parity 主动取消 barrier，并用 debugfs 暂停点确定性验证 queued P1/P2 被取消且计数无泄漏。

**Architecture:** 每个 data block 维护 `cancelling` 和 parity work pending 计数；erase 两阶段执行，先独占目标 block 的 cancel barrier、禁止新请求并等待目标 pending 清零，再擦除介质并推进 generation。同 block 后来的 erase 返回 `-EBUSY`，不释放前一个 owner 的 barrier。Parity worker 在 claim 前响应 cancel，统一终态函数释放 scheduler reservation 和 block pending。Debugfs 只在 worker 未持锁、未 claim 时暂停，不参与正式正确性路径。

**Tech Stack:** Linux 7.0.12、MTD direct callbacks、kernel workqueue/mutex/spinlock/atomic/waitqueue、debugfs、KUnit、QEMU 11.0.2 guest smoke。

## Global Constraints

- 工作区固定为 `/Users/yangyu/Documents/3dnand-page-raid-serial-worktree`，分支固定为 `codex/page-raid-serial-async-priority`。
- 不修改 `include/linux/mtd/mtd.h`、`drivers/mtd/mtdcore.c`、`drivers/mtd/ubi/*` 或 `fs/ubifs/*`。
- 串行布局固定为同一物理 block 内 `D0..D6,P`，不得跳页或降序 program。
- 前台优先级保持 P0，rebuild read 保持 P1，parity program 保持 P2。
- Debugfs 暂停点不得持有 `mtd_lock` 或 scheduler spinlock。
- Erase 返回成功时目标 block pending 必须为 0，旧 generation parity 不得再 program。
- 同 block erase 的 cancel barrier 只有一个 owner；未取得所有权的 erase 必须
  设置 `fail_addr` 并返回 `-EBUSY`，不得调用 `cancel_end()`。
- 正常 cancel 不增加 `raid_failed`；generation 不匹配仍增加 `parity_stale`。
- 所有新增行为必须执行 RED→GREEN，阶段提交前运行静态测试、模块构建、KUnit 和 guest smoke。

---

## File Structure

```text
linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h
    q3n_block_barrier 类型和纯状态机接口
linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c
    barrier 纯 helper、scheduler 统计快照
linux/drivers/mtd/nand/raw/qemu_3dnand_main.c
    per-block 生命周期、erase 两阶段 barrier、worker cancel、debugfs pause
linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c
    barrier 状态与计数 KUnit
rootfs/profile.d/mtd.sh
    q3n-cancel-barrier-smoke guest 验收
tests/test_scripts.sh
    新接口和 guest 命令结构门禁
docs/superpowers/plans/2026-07-11-3dnand-page-raid-driver-implementation.md
    总计划状态更新
```

---

### Task 1: 实现可单测的 per-block barrier 状态机

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`

**Interfaces:**
- Produces: `struct q3n_block_barrier`。
- Produces: `q3n_block_barrier_init()`、`q3n_block_parity_get()`、`q3n_block_parity_put()`、`q3n_block_cancel_begin()`、`q3n_block_cancel_end()`、`q3n_block_is_cancelling()`、`q3n_block_pending()`。

- [x] **Step 1: 写失败的 KUnit**

在 `qemu_3dnand_kunit.c` 增加：

```c
static void q3n_block_barrier_blocks_new_work_and_drains_test(struct kunit *test)
{
	struct q3n_block_barrier barrier;

	q3n_block_barrier_init(&barrier);
	KUNIT_EXPECT_EQ(test, q3n_block_pending(&barrier), 0);
	KUNIT_ASSERT_EQ(test, q3n_block_parity_get(&barrier), 0);
	KUNIT_EXPECT_EQ(test, q3n_block_pending(&barrier), 1);
	KUNIT_ASSERT_EQ(test, q3n_block_cancel_begin(&barrier), 0);
	KUNIT_EXPECT_TRUE(test, q3n_block_is_cancelling(&barrier));
	KUNIT_EXPECT_EQ(test, q3n_block_cancel_begin(&barrier), -EBUSY);
	KUNIT_EXPECT_EQ(test, q3n_block_parity_get(&barrier), -EBUSY);
	KUNIT_EXPECT_TRUE(test, q3n_block_parity_put(&barrier));
	KUNIT_EXPECT_EQ(test, q3n_block_pending(&barrier), 0);
	q3n_block_cancel_end(&barrier);
	KUNIT_EXPECT_FALSE(test, q3n_block_is_cancelling(&barrier));
}
```

- [x] **Step 2: 构建并确认 RED**

Run:

```bash
./scripts/shell.sh ./scripts/apply-linux-overlay.sh
./scripts/shell.sh make -C /workspace/work/linux/linux-7.0.12 \
  O=/workspace/work/build/linux-7.0.12 ARCH=x86_64 \
  CROSS_COMPILE=x86_64-linux-gnu- M=drivers/mtd/nand/raw modules
```

Expected: modpost/compile 因 `q3n_block_barrier_*` 未定义失败。

- [x] **Step 3: 定义类型和最小实现**

在 `qemu_3dnand_priv.h` 定义：

```c
struct q3n_block_barrier {
	atomic_t pending_parity;
	bool cancelling;
};
```

在 `qemu_3dnand_sched.c` 实现：

```c
void q3n_block_barrier_init(struct q3n_block_barrier *barrier)
{
	atomic_set(&barrier->pending_parity, 0);
	barrier->cancelling = false;
}

int q3n_block_parity_get(struct q3n_block_barrier *barrier)
{
	if (barrier->cancelling)
		return -EBUSY;
	atomic_inc(&barrier->pending_parity);
	return 0;
}

bool q3n_block_parity_put(struct q3n_block_barrier *barrier)
{
	return atomic_dec_and_test(&barrier->pending_parity);
}
```

`cancel_begin` 返回所有权获取结果：首次返回 0，已 cancelling 返回
`-EBUSY` 且不改变状态；`cancel_begin/end` 使用 `WRITE_ONCE()`，
`is_cancelling` 使用 `READ_ONCE()`；
调用者负责用 `mtd_lock` 串行化 cancelling 与新的 `get()`。Worker 的无锁
暂停条件只通过 `is_cancelling` 读取。

- [x] **Step 4: 运行 GREEN 验证**

Run: Task 1 Step 2 的 raw NAND 模块构建命令。

Expected: exit 0。

- [x] **Step 5: 提交**

```bash
git add linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h \
        linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c \
        linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c
git commit -m "feat: add per-block parity cancel state"
```

---

### Task 2: 接入 worker 终态回收和 erase 两阶段 barrier

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_kunit.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Consumes: Task 1 `q3n_block_barrier` API。
- Produces: `qemu_3dnand_finish_parity_work()` 统一终态回收。
- Produces: `qemu_3dnand_cancel_block_parity()` erase barrier。

- [x] **Step 1: 增加失败的结构门禁和计数测试**

在 `tests/test_scripts.sh` 增加：

```sh
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_finish_parity_work'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'qemu_3dnand_cancel_block_parity'
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c 'wait_event.*pending_parity'
```

在 KUnit 增加一次 `get()` 后分别模拟 success/cancel 的两个 `put()` 测试，
断言每个独立 barrier 仅在计数从 1 到 0 时返回 true。

- [x] **Step 2: 运行并确认 RED**

Run: `./scripts/smoke-test.sh`

Expected: FAIL on `qemu_3dnand_finish_parity_work`。

- [x] **Step 3: 把 barrier 放入 block metadata**

```c
struct qemu_3dnand_data_block_meta {
	u32 generation;
	bool bad;
	bool erased;
	struct q3n_block_barrier parity_barrier;
};
```

Probe 初始化每个 block 时调用 `q3n_block_barrier_init()`。

- [x] **Step 4: 在创建 parity work 时计入 block pending**

在 `qemu_3dnand_queue_parity_locked()` 完成 stripe reservation 后、分配 work
前调用：

```c
ret = q3n_block_parity_get(&q3n->data_meta[block].parity_barrier);
if (ret) {
	q3n_sched_release_parity(&q3n->sched);
	return ret;
}
```

所有后续分配/入队失败路径必须调用一次 `q3n_block_parity_put()`。

- [x] **Step 5: 提取统一终态函数**

```c
static void qemu_3dnand_finish_parity_work(
		struct qemu_3dnand_parity_work *parity)
{
	struct qemu_3dnand *q3n = parity->q3n;
	struct q3n_block_barrier *barrier =
		&q3n->data_meta[parity->block].parity_barrier;

	q3n_sched_release_parity(&q3n->sched);
	if (q3n_block_parity_put(barrier))
		wake_up_all(&q3n->parity_cancel_waitq);
	kfree(parity->page_buf);
	kfree(parity->rebuild.parity_accumulator);
	kfree(parity);
}
```

Success、stale、cancel 和 error 全部进入该函数；删除分散的 reservation 和
buffer 释放。

- [x] **Step 6: Worker 在 claim 前处理 cancelling**

Worker 获取 `mtd_lock` 后先检查：

```c
if (q3n_block_is_cancelling(barrier)) {
	if (parity->request_queued)
		q3n_sched_cancel(&q3n->sched, &parity->request);
	parity->request_queued = false;
	mutex_unlock(&q3n->mtd_lock);
	qemu_3dnand_finish_parity_work(parity);
	return;
}
```

Cancel 不增加 `raid_failed` 或 `parity_stale`。

- [x] **Step 7: 实现 erase 两阶段 barrier**

初始化 `init_waitqueue_head(&q3n->parity_cancel_waitq)`。每个 block erase：

```c
ret = q3n_block_cancel_begin(barrier);
if (ret)
	return ret;
mutex_unlock(&q3n->mtd_lock);
wait_event(q3n->parity_cancel_waitq,
	   q3n_block_pending(barrier) == 0);
mutex_lock(&q3n->mtd_lock);
ret = qemu_3dnand_erase_phys_block_locked(q3n, block);
/* 成功时再推进 generation/清 validation/reset next_prog_page。 */
q3n_block_cancel_end(barrier);
```

所有已取得 barrier 所有权的物理 erase 失败和循环退出路径都必须执行
`cancel_end()`；未取得所有权的同 block erase 设置 `fail_addr` 后返回
`-EBUSY`，绝不执行 `cancel_end()`。

- [x] **Step 8: 防御 barrier 解锁窗口中的前台访问**

`_read/_write` 在解析 block 并持有 `mtd_lock` 后，如果 cancelling，释放锁并
返回 `-EBUSY`；不得留下 queued P0 request。由于 P0 已在 lock helper 内摘队，
只需终止本次回调。

- [x] **Step 9: 构建和静态验证**

Run:

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/apply-linux-overlay.sh
./scripts/shell.sh make -C /workspace/work/linux/linux-7.0.12 \
  O=/workspace/work/build/linux-7.0.12 ARCH=x86_64 \
  CROSS_COMPILE=x86_64-linux-gnu- M=drivers/mtd/nand/raw modules
```

Expected: both exit 0。

- [x] **Step 10: 提交**

```bash
git add linux/drivers/mtd/nand/raw tests/test_scripts.sh
git commit -m "feat: cancel block parity before erase"
```

---

### Task 3: 增加 claim 前 debugfs 暂停和计数观测

**Files:**
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_main.c`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_priv.h`
- Modify: `linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c`
- Modify: `tests/test_scripts.sh`

**Interfaces:**
- Produces debugfs: `parity_pause_block`、`parity_pause_enable`、
  `parity_paused`、`pending_parity`、`reserved_parity`。
- Produces: `q3n_sched_get_counts(struct q3n_sched *, u32 *, u32 *)`。

- [x] **Step 1: 添加失败的结构测试**

```sh
for name in parity_pause_block parity_pause_enable parity_paused \
            pending_parity reserved_parity; do
  assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_main.c \
    "debugfs_create_file.*$name"
done
assert_contains linux/drivers/mtd/nand/raw/qemu_3dnand_sched.c 'q3n_sched_get_counts'
```

- [x] **Step 2: 运行确认 RED**

Run: `./scripts/smoke-test.sh`

Expected: FAIL on `parity_pause_block`。

- [x] **Step 3: 实现 scheduler 计数快照**

```c
void q3n_sched_get_counts(struct q3n_sched *sched, u32 *pending, u32 *reserved)
{
	unsigned long flags;

	spin_lock_irqsave(&sched->lock, flags);
	*pending = sched->pending_parity;
	*reserved = sched->reserved_parity;
	spin_unlock_irqrestore(&sched->lock, flags);
}
```

- [x] **Step 4: 增加暂停状态和 setter/getter**

设备对象增加：

```c
wait_queue_head_t parity_pause_waitq;
atomic_t parity_paused;
u32 parity_pause_block;
bool parity_pause_enable;
```

`pause_enable=0`、目标 block cancelling、或模块 remove 时调用
`wake_up_all(&q3n->parity_pause_waitq)`。

- [x] **Step 5: Worker 在 claim 和 mtd_lock 前进入暂停点**

```c
if (READ_ONCE(q3n->parity_pause_enable) &&
    READ_ONCE(q3n->parity_pause_block) == parity->block &&
    !q3n_block_is_cancelling(barrier)) {
	atomic_inc(&q3n->parity_paused);
	wait_event(q3n->parity_pause_waitq,
		!READ_ONCE(q3n->parity_pause_enable) ||
		READ_ONCE(q3n->parity_pause_block) != parity->block ||
		q3n_block_is_cancelling(barrier));
	atomic_dec(&q3n->parity_paused);
}
```

不得在此代码段持有 `mtd_lock` 或 scheduler spinlock。

- [x] **Step 6: 修正 remove 顺序**

Remove 先 `debugfs_remove_recursive()` 阻止新的测试控制，再关闭 pause 并唤醒
worker；随后 `mtd_device_unregister()` 阻止新的 MTD 入口，再执行
`flush_workqueue()`/`destroy_workqueue()`，最后释放 metadata。该顺序保证
unregister 触发同步时没有 worker 被测试钩子暂停。

- [x] **Step 7: 运行静态测试和模块构建**

Run: Task 2 Step 9 的全部命令。

Expected: exit 0。

- [x] **Step 8: 提交**

```bash
git add linux/drivers/mtd/nand/raw tests/test_scripts.sh
git commit -m "test: add deterministic parity pause hook"
```

---

### Task 4: Guest 确定性 cancel 验收和总计划更新

**Files:**
- Modify: `rootfs/profile.d/mtd.sh`
- Modify: `tests/test_scripts.sh`
- Modify: `docs/superpowers/plans/2026-07-11-3dnand-page-raid-driver-implementation.md`

**Interfaces:**
- Produces guest command: `/etc/profile.d/mtd.sh q3n-cancel-barrier-smoke`。

- [x] **Step 1: 添加失败的命令门禁**

```sh
assert_contains rootfs/profile.d/mtd.sh 'q3n-cancel-barrier-smoke'
```

Run: `./scripts/smoke-test.sh`

Expected: FAIL on `q3n-cancel-barrier-smoke`。

- [x] **Step 2: 实现 guest 命令**

命令必须：

```sh
echo 0 > "$stats/parity_pause_block"
echo 1 > "$stats/parity_pause_enable"
flash_erase -q "$mtd_dev" 0 1
dd if=/dev/zero of=/tmp/q3n-cancel.bin bs=16384 count=7 2>/dev/null
dd if=/tmp/q3n-cancel.bin of="$mtd_dev" bs=16384 count=7 2>/tmp/q3n-cancel-dd.err &
cancel_writer_pid=$!
```

轮询 `parity_paused > 0` 后记录 `parity_written`、`raid_failed`，执行 erase，
然后 `wait "$cancel_writer_pid"` 回收后台 writer；
随后断言：

```sh
[ "$(cat "$stats/parity_written")" -eq "$written_before" ]
[ "$(cat "$stats/raid_failed")" -eq "$failed_before" ]
[ "$(cat "$stats/pending_parity")" -eq 0 ]
[ "$(cat "$stats/reserved_parity")" -eq 0 ]
```

无论中间哪一步失败，函数返回前都执行：

```sh
echo 0 > "$stats/parity_pause_enable" 2>/dev/null || true
```

最后调用 `mtd_q3n_serial_smoke` 验证新 generation 可写和可恢复。

- [x] **Step 3: 更新总计划**

把 Task 9 group cancel barrier 标记完成，记录确定性结果需要包含：

```text
parity_paused > 0
parity_written unchanged
raid_failed unchanged
pending_parity=0
reserved_parity=0
```

持久化坏块能力仍保留为未完成范围。

- [x] **Step 4: 完整构建**

```bash
./scripts/smoke-test.sh
./scripts/shell.sh ./scripts/apply-linux-overlay.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
```

Expected: all exit 0。

- [x] **Step 5: Guest 验收**

启动：

```bash
./scripts/run-qemu.sh
```

Guest 执行：

```sh
modprobe qemu_3dnand_test
/etc/profile.d/mtd.sh q3n-cancel-barrier-smoke
/etc/profile.d/mtd.sh q3n-generation-smoke
/etc/profile.d/mtd.sh q3n-serial-smoke
```

Expected:

```text
KUnit totals: fail:0
q3n cancel barrier smoke passed
q3n generation smoke passed
q3n serial smoke passed
```

- [x] **Step 6: 请求复审**

审查重点：erase 解锁等待窗口、cancel/worker 竞态、pending/reservation 恰好
释放一次、debugfs pause/remove 死锁和 MTD remove 顺序。

- [x] **Step 7: 提交**

```bash
git add rootfs/profile.d/mtd.sh tests/test_scripts.sh \
        docs/superpowers/plans/2026-07-11-3dnand-page-raid-driver-implementation.md
git commit -m "test: verify erase parity cancel barrier"
```

- [x] **后续：最终修正和验收完成后统一推送分支**

---

## Final Acceptance Checklist

- [x] `git diff --check` 无输出。
- [x] `./scripts/smoke-test.sh` 通过。
- [x] Raw NAND 模块和完整 Linux 7.0.12 构建退出 0。
- [x] KUnit 全部通过且新增 barrier case 可见。
- [x] Pause worker 未持有 `mtd_lock`/scheduler spinlock。
- [x] Erase 返回时目标 block pending 为 0。
- [x] Cancel stripe 不增加 `parity_written` 或 `raid_failed`。
- [x] `pending_parity` 和 `reserved_parity` 回到 0。
- [x] 新 generation 严格升序写和 Page-RAID 恢复通过。
- [x] 分支已推送且工作区干净。
