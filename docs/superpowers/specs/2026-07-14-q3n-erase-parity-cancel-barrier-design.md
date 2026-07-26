# QEMU 3D NAND Erase/Parity Cancel Barrier Design

## 目标

在不修改 MTD/UBI core、不引入 FTL/GC 的前提下，为串行 `D0..D6,P`
Page-RAID profile 增加按物理 block 隔离的 erase barrier：erase 开始后，
该 block 已排队但尚未提交的 P1 rebuild/P2 parity 必须取消；erase 必须等
这些后台对象释放 reservation 后才能擦除介质。其他 block 的前后台访问不
因该 barrier 被全局排空。

同时增加仅用于测试的 debugfs 暂停钩子，确定性制造“parity 已排队但尚未
claim”的状态，验证取消路径，而不是依赖 workqueue 时序碰撞。

## 非目标

- 不修改 Linux MTD、UBI 或 UBIFS 源码。
- 不增加多 LUN/多 plane 并行。
- 不实现坏块替换、FTL、GC 或磨损均衡。
- 不为生产路径引入 debugfs 依赖；debugfs 只控制测试暂停。
- 不在本阶段增加持久化 markbad 控制器命令。

## 方案选择

采用“per-block cancelling 状态 + per-block pending 计数 + 全局等待队列”。
不使用全局 `flush_workqueue()`，因为它会让一个 block 的 erase 等待所有
block；也不依赖 generation 的事后自清理，因为 erase 返回时无法证明目标
block 的 reservation 已归零。

## 数据结构

每个 `qemu_3dnand_data_block_meta` 增加：

- `bool cancelling`：禁止目标 block 创建或执行新的 data/parity 操作。
- `atomic_t pending_parity`：该 block 尚未终态回收的 parity work 数。

设备对象增加：

- `wait_queue_head_t parity_cancel_waitq`：erase 等待目标 block 的 pending
  计数归零；worker 终态回收时唤醒。
- debugfs 暂停状态：`pause_enabled`、`pause_block`、`paused_workers` 和
  `parity_pause_waitq`。

全局 scheduler 的 `reserved_parity` 继续按 stripe 计数。每个 parity work
创建成功时同时增加目标 block 的 `pending_parity`；无论成功、失败、stale
或 cancel，统一终态函数各释放一次 block pending 和 scheduler reservation。

## Erase Barrier 数据流

对每个待擦除逻辑 block：

1. 获取 `mtd_lock`，以 `q3n_block_cancel_begin()` 获取目标 block 的 barrier
   所有权并设置 `data_meta[block].cancelling = true`。若同 block 已有 erase
   owner，则后来的 erase 返回 `-EBUSY` 并设置 `fail_addr`，不得修改
   cancelling，也不得调用 `cancel_end()`。
2. 唤醒 debugfs 暂停等待，使目标 block 的 worker 能观察 cancelling。
3. 释放 `mtd_lock`，等待 `pending_parity == 0`。
4. Worker 被调度后，在 claim 前看到 cancelling：若请求仍在 P1/P2 队列，
   调用 `q3n_sched_cancel()` 原子摘队，然后进入统一终态回收；不得执行 NAND
   read/program，不增加 `raid_failed`。
5. Erase 重新获取 `mtd_lock`，确认 pending 仍为 0，执行物理 block erase。
6. 仅当物理 erase 成功时推进非零 generation，清空 data validation、重置
   `next_prog_page`，并使旧 parity index stale。
7. 清除 cancelling 并唤醒等待者。物理 erase 失败也必须清除 cancelling；
   只有成功取得 barrier 的 erase owner 可以执行该操作。已取消的未提交
   stripe 保持 unprotected，不伪造 parity 成功。

前台 read/write 在取得 `mtd_lock` 后检查 cancelling。正常 MTD core 会串行化
这些入口；该检查作为防御，在 barrier 的主动解锁等待窗口内返回 `-EBUSY`，
避免新请求进入目标 block。

## Worker 状态与错误处理

Worker 的 claim 前检查顺序固定为：

1. 测试暂停条件；若目标 block cancelling，暂停必须被绕过。
2. 获取 `mtd_lock`。
3. 若 cancelling：取消 queued request，记为正常 cancel，释放锁并终态回收。
4. 校验 generation；不匹配返回 stale，增加 `parity_stale`。
5. 执行既有 P0/P1/P2 claim 和单页 I/O。

正常 cancel 不增加 `raid_failed`。所有终态路径必须满足：

- scheduler request 不再挂链；
- block pending 恰好减一；
- scheduler reservation 恰好释放一次；
- worker 私有 buffer 恰好释放一次；
- pending 变为 0 时唤醒 erase。

## Debugfs 确定性暂停钩子

增加以下测试接口：

- `parity_pause_block`：目标物理 block 编号。
- `parity_pause_enable`：1 在 claim 前暂停，0 恢复。
- `parity_paused`：当前进入暂停点的 worker 数。
- `pending_parity`、`reserved_parity`：只读全局统计，用于检查终态平衡。

暂停发生在 worker 未持有 `mtd_lock`、未 claim scheduler request 时。Erase
设置 cancelling 后会唤醒暂停 worker；因此测试钩子不会阻塞正式 cancel
barrier。模块 remove/debugfs 删除前必须关闭暂停并唤醒所有 worker。

## 测试策略

### KUnit

- block pending 从 1 进入 cancel 后归零并触发 wake 条件。
- cancelling parity request 从 P1/P2 队列安全摘除。
- cancel 不改变 generation；只有成功 erase 推进 generation。
- reservation/pending 在 success、stale、cancel 和 enqueue failure 路径平衡。
- 同 block 第一次 cancel begin 成功，第二次返回 `-EBUSY`；owner end 前
  cancelling 保持为 true，owner end 后可再次取得 barrier。

### Guest 确定性验收

1. Erase block 0，启用 block 0 parity pause。
2. 连续写 D0..D6，轮询 `parity_paused > 0`。
3. 记录 `parity_written` 与 `raid_failed`，执行 block 0 erase。
4. 断言 erase 返回；`parity_written` 未增加、`raid_failed` 未增加、
   `pending_parity == 0`、`reserved_parity == 0`。
5. 关闭暂停，重新连续写 8 页，注入 data loss，验证新 generation 的 parity
   恢复成功。

### 回归

- 静态脚本门禁。
- raw NAND 模块构建。
- 完整 Linux/rootfs 构建。
- 既有 KUnit、`q3n-serial-smoke`、`q3n-generation-smoke`。

## 完成标准

- 确定性 guest 用例证明 erase 时目标 P1/P2 尚在队列且被取消。
- Erase 返回时目标 block pending 为 0，全局 reservation 无泄漏。
- 旧 stripe 的 parity program 未发生，且正常 cancel 不计入 RAID failure。
- 新 generation 可继续严格升序编程并完成 RAID 恢复。
- 文档将 Task 9 的 group cancel barrier 标记完成，同时保留持久化坏块能力
  和并行 profile 为后续任务。
