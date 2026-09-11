# Linux 内核 MTD QEMU 开发环境

这个仓库用于搭建一个可复现的 Linux 内核态开发环境，支持编译最新稳定版 Linux 内核、开发树外内核模块、调试 MTD 相关驱动，并通过 QEMU 启动验证。

默认流程使用 Docker 封装 Linux 构建工具链，因此 macOS 和 Linux 上的操作基本一致。
在 Apple Silicon macOS 上，容器会使用 x86_64 交叉编译工具链构建 QEMU 默认内核，并把 amd64 版 BusyBox 放入 initramfs，避免 x86_64 guest 执行 arm64 用户态程序。

## 前置条件

宿主机需要安装 Docker。macOS 推荐使用 Docker Desktop；Linux 推荐使用发行版自带 Docker 或 Docker Engine。

## 快速开始

构建工具镜像：

```sh
./scripts/build-image.sh
```

如果访问 Docker Hub 超时，可以指定可访问的 Ubuntu 基础镜像，例如公司内网镜像或自建代理：

```sh
BASE_IMAGE=your-registry.example.com/library/ubuntu:24.04 ./scripts/build-image.sh
```

进入容器：

```sh
./scripts/shell.sh
```

在容器内获取当前 stable Linux 内核：

```sh
./scripts/fetch-linux.sh
```

也可以指定版本：

```sh
./scripts/fetch-linux.sh --version 7.0.12
```

获取 QEMU 源码并编译带 q3n-nand overlay 的 QEMU：

```sh
./scripts/fetch-qemu.sh
./scripts/build-qemu.sh
```

默认下载 QEMU 11.0.2，源码放在 `work/qemu/qemu-11.0.2`，构建输出在 `work/build/qemu-11.0.2/qemu-system-x86_64-unsigned`。
可以用 `QEMU_VERSION` 或 `QEMU_DIR` 覆盖默认源码版本/目录。
当前 QEMU overlay 会注册 `q3n-nand-pci`，`scripts/run-qemu.sh` 默认把它挂到 q35 PCI 总线上。
Linux overlay 会注册 `qemu_3dnand` PCI 驱动，并通过直接 MTD 回调暴露名为 `qemu-3dnand` 的 MTD 设备。

配置并编译内核：

```sh
./scripts/configure-kernel.sh
./scripts/build-kernel.sh
```

编译样例 MTD 模块：

```sh
./scripts/build-module.sh
```

构建 initramfs：

```sh
./scripts/build-rootfs.sh
```

启动 QEMU：

```sh
./scripts/run-qemu.sh
```

`run-qemu.sh` 默认复用 `work/media/q3n-nand.raw`，因此物理 NAND 的 main、
OOB、坏块标记和 bitflip overlay 会在 QEMU 正常退出后保留。需要全新擦除态介质
时使用：

```sh
./scripts/run-qemu.sh --fresh-nand
```

也可以用 `--nand-image PATH` 选择独立镜像。镜像是约 51 GiB 的固定布局 sparse
raw 文件，实际只为 header、已写物理页和 overlay 分配空间。

每个 `0x4680` B 物理页的精确布局为：

```text
0x0000..0x3fff main
0x4000         OOB head / BBM / logical OOB[0]
0x4001..0x4600 LDPC
0x4601..0x467f OOB tail / logical OOB[1..127]
```

命令 6/7 各自只传输 128 B logical OOB，不携带 main，也不向 guest 暴露
LDPC。Linux 标坏通过普通 OOB PROGRAM 将 logical OOB byte 0 编程为 `00`；
没有专用 mark-bad 命令。OOB PROGRAM 保留 main 与 LDPC，main PROGRAM
保留 OOB head/tail。

## Multi-plane Page RAID1 / RAID5

设计更新（2026-09-11）：后续拟迁移至 NAND core 的 `nand_chip.ecc.*`
接口，并取消 RAID manifest，改用代码固定计算 parity / mirror 位置。
无持久化提交证据时，重启后不自动恢复不可纠数据；完整限制见
[ECC 与固定页映射设计](docs/superpowers/specs/2026-08-30-multiplane-page-raid1-raid5-design.md)
及 [迁移任务清单](docs/superpowers/plans/2026-08-30-multiplane-page-raid1-raid5-implementation.md)。
目前仅更新设计，当前代码仍使用 direct MTD 回调和 manifest；以下说明描述现有实现。

控制器固定为 2 die × 4 plane，冗余成员始终位于同一 die，不做跨 die
备份。加载驱动时用只读参数选择 profile：

```sh
modprobe qemu_3dnand raid_level=1   # plane0/1、plane2/3 镜像，16 KiB 写对齐
modprobe qemu_3dnand                # 默认 RAID5，3D+1P，48 KiB 写对齐
```

RAID5 的 parity plane 按 stripe ID 在 0、1、2、3 间轮转。主数据通过一次
multi-plane PROGRAM 写入；所有主成员成功后，再用独立的 multi-plane OOB
PROGRAM 发布 v2 manifest。OOB byte 0 仍是 BBM，manifest 从 byte 1 开始，
这 127 B 是驱动私有区域，不作为公共 MTD OOB 暴露。旧串行 D0..D6/P 介质
没有 v2 manifest，切换到新 profile 时应使用 `--fresh-nand`。

验收命令：

```sh
./scripts/q3n-raid1-smoke.sh
./scripts/q3n-raid5-smoke.sh
./scripts/q3n-kunit-smoke.sh
./scripts/q3n-persistence-smoke.sh
```

原始介质跨重启持久性验收命令为：

```sh
./scripts/q3n-persistence-smoke.sh
```

脚本执行两轮 guest，验证 RAID5 v2 manifest 数据和同 die 四 plane 坏块组
状态跨重启保留。QEMU 不解释 RAID 布局；映射、manifest 和恢复策略始终由
Linux 驱动管理。

自动执行 MTD smoke 并在成功后关闭虚拟机：

```sh
./scripts/run-qemu.sh --append "MTD_SMOKE=1"
```

自动执行 nandsim + UBI/UBIFS 挂载测试，并在成功后关闭虚拟机：

```sh
./scripts/run-qemu.sh --append "MTD_SMOKE=ubifs"
```

进入 guest 后可以运行：

```sh
/etc/profile.d/mtd.sh smoke
/etc/profile.d/mtd.sh nandsim
/etc/profile.d/mtd.sh ubifs
modprobe mtd_demo
dmesg
```

## GDB 调试

启动等待 GDB 的 QEMU：

```sh
./scripts/run-qemu.sh --debug
```

另开一个容器终端连接 GDB：

```sh
./scripts/gdb-kernel.sh
```

调试模式会使用 QEMU 的 GDB stub，也就是 `-s -S`。内核命令行默认包含 `nokaslr`，便于符号地址稳定。

## MTD 仿真路径

第一阶段使用 Linux 内核自带的模拟设备：

- `mtdram`：创建 RAM-backed MTD 设备。
- `nandsim`：模拟 NAND 设备。
- `mtd-utils`：配合 UBI/UBIFS 做读写、擦除和挂载测试。

这条路径不依赖特定 QEMU machine 暴露 Flash，因此最容易跑通。

在 QEMU guest 内执行：

```sh
/etc/profile.d/mtd.sh ubifs
```

脚本会自动加载 `nandsim`、`ubi`、`ubifs`，找到 NAND simulator 对应的 `/dev/mtdX`，执行 `flash_erase`、`ubiformat`、`ubiattach`、`ubimkvol`，最后将 `ubi0:rootfs` 挂载到 `/mnt/ubifs` 并写入测试文件。

第二阶段可以新增 QEMU 板级 Flash profile，例如 NOR 或 NAND 仿真。相关参数应放到 `configs/qemu/`，不要混入默认 x86_64 流程。

## 目录说明

- `Dockerfile`：构建 Linux 内核、QEMU、GDB 和 MTD 工具环境。
- `Dockerfile` 也会提取 `/opt/rootfs-amd64/usr/bin/busybox`，用于 x86_64 initramfs。
- `scripts/`：获取源码、编译、构建 rootfs、运行 QEMU 和连接 GDB。
- `configs/linux/`：内核配置片段。
- `configs/linux/qemu-x86_64-lean.fragment`：关闭图形、声音、无线、NFS 等无关大子系统，避免 Docker Desktop 上 debug 内核链接时内存不足。
- `configs/qemu/`：QEMU profile。
- `qemu/`：q3n-nand QEMU 源码 overlay，可通过 `scripts/apply-qemu-overlay.sh` 合入 `work/qemu/qemu-*`。
- `linux/`：qemu_3dnand Linux 驱动 overlay，可通过 `scripts/apply-linux-overlay.sh` 合入 `work/linux/linux-*`。
- `rootfs/`：initramfs 模板。
- `drivers/mtd_demo/`：树外 MTD 示例模块。
- `work/`：下载和构建产物目录，已被 Git 忽略。

## 本地轻量验证

不下载内核、不启动 Docker 的结构验证：

```sh
./scripts/smoke-test.sh
```

完整验证需要 Docker、网络和较长编译时间：

```sh
./scripts/build-image.sh
./scripts/shell.sh ./scripts/fetch-linux.sh
./scripts/shell.sh ./scripts/fetch-qemu.sh
./scripts/shell.sh ./scripts/build-qemu.sh
./scripts/shell.sh ./scripts/configure-kernel.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-module.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-persistence-smoke.sh
./scripts/shell.sh ./scripts/run-qemu.sh --append "MTD_SMOKE=1"
./scripts/shell.sh ./scripts/run-qemu.sh --append "MTD_SMOKE=ubifs"
```

串行同块 `D0..D6,P` Page-RAID 的确定性端到端验收可直接运行：

```sh
Q3N_ENABLE_MULTIPLANE_RAID=0 ./scripts/shell.sh ./scripts/configure-kernel.sh
Q3N_ENABLE_MULTIPLANE_RAID=0 ./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-serial-smoke.sh
```

`Q3N_ENABLE_MULTIPLANE_RAID` 是编译期宏，默认值为 `1`。默认模式使用同一
die 内的多-plane RAID1/RAID5，并由驱动私有地占用 OOB；设为 `0` 时恢复原有
串行 `D0..D6,P` 路径、16 KiB writesize 和 128 B 公共 OOB。切换回默认模式时
省略该环境变量并重新执行配置、内核构建和 rootfs 构建即可。`raid_level` 仅在
宏值为 `1` 时决定 RAID1 或 RAID5 profile。

该 host wrapper 每次使用 fresh NAND，并严格要求 guest 同时输出串行验收和
通用 MTD 成功 marker；单凭 QEMU 正常退出不会判定成功。测试逐页使用不同的
确定性内容，验证真实 worker 的 P0>P1>P2 continuation、parity queue setup
失败、one-shot program failure 及其 clear/invalid/reset disarm 路径，并确认
失败后已写 data 仍可读、后续 stripe 可正常保护。guest 内可单独查看从启动
以来的累计统计：

```sh
/etc/profile.d/mtd.sh q3n-parity-stats
```

输出包括 `foreground_ops`、`parity_reads`、`parity_writes`、
`protected_stripes`、`unprotected_stripes`、`failed_stripes` 和
`max_pending_parity`。前三类物理命令由 QEMU 计数；stripe 状态事件和 pending
高水位由 Linux 驱动计数。

## 常见问题

如果提示缺少 Docker，请先安装并启动 Docker。

如果 kernel.org 下载失败，可以稍后重试，或手动指定已经下载并解压的源码目录：

```sh
LINUX_DIR=/workspace/work/linux/linux-7.0.12 ./scripts/configure-kernel.sh
```

如果 kernel.org 下载速度很慢，可以指定内核镜像源：

```sh
KERNEL_BASE_URL=https://mirrors.tuna.tsinghua.edu.cn/kernel ./scripts/shell.sh ./scripts/fetch-linux.sh --redownload
```

如果 QEMU 调试端口被占用，请关闭已有 QEMU 进程，或修改脚本中的 GDB 端口参数。
