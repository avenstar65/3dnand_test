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
Linux overlay 会注册 `qemu_3dnand` PCI 驱动：先读取完整 8-byte NAND ID，
从 `ytmc_nand.c` 白名单取得 `nand_flash_dev` 表，再调用
`nand_scan_with_ids()`。MTD 读写、擦除、坏块和 BBT 均由 Raw NAND Core
提供。Q3N 驱动使用传统 `cmdfunc()`、`waitfunc()`、`read_byte()` 等
legacy callbacks 处理 RESET、READID、STATUS 和 ERASE，使用
`chip->ecc.*` 处理 page/OOB/raw 数据，并通过 `setup_read_retry()` 接入
NAND Core 的 read retry；不注册 controller `exec_op()`。

配置并编译内核。默认是 identity；也可显式选择 identity、Page RAID 2:1/4:1/8:1
或 fixed four-plane：

```sh
./scripts/configure-kernel.sh
./scripts/configure-kernel.sh --q3n-mode page-raid-4
./scripts/configure-kernel.sh --q3n-mode multiplane
./scripts/build-kernel.sh
```

编译样例 MTD 模块：

```sh
./scripts/build-module.sh
```

构建 initramfs：

```sh
./scripts/shell.sh ./scripts/build-rootfs.sh
```

启动 QEMU：

```sh
./scripts/run-qemu.sh
```

`run-qemu.sh` 默认以 identity mode 复用 `work/media/q3n-nand.raw`，因此物理 NAND 的 main、
OOB、坏块标记和 bitflip overlay 会在 QEMU 正常退出后保留。需要全新擦除态介质
时使用：

```sh
./scripts/run-qemu.sh --fresh-nand
```

也可以用 `--nand-image PATH` 选择独立镜像。当前 v2 物理镜像的固定逻辑长度为
116,549,226,496 B（约 108.5 GiB）；它仍是 sparse raw 文件，实际分配通常远小于
逻辑长度，只包含 header、已写物理页和 overlay。

four-plane mode 必须配合 multi-plane 内核配置，默认使用独立的
`work/media/q3n-nand-multiplane.raw`，不会迁移、删除或复用旧镜像：

```sh
./scripts/run-qemu.sh --nand-mode multiplane
./scripts/run-qemu.sh --nand-mode multiplane --fresh-nand
./scripts/run-qemu.sh --nand-mode page-raid
```

`--nand-mode` 只接受 `identity`、`page-raid` 或 `multiplane`；启动前会检查
已构建内核的匹配 Kconfig。multi-plane 向 NAND Core 注册 65536 B page、4096 B
OOB、104857600 B eraseblock 和 416 logical blocks。它一次提交固定 plane0..3
group；这是功能性接口，不代表真实 NAND 并行性能、回滚或掉电原子性。

每个 `0x4a00` B 物理页的精确布局为：

```text
0x0000..0x3fff main
0x4000         OOB head / BBM / logical OOB[0]
0x4001..0x4600 LDPC
0x4601..0x49ff OOB tail / logical OOB[1..1023]
```

命令 6/7 各自只传输 1024 B logical OOB，不携带 main，也不向 guest 暴露
LDPC。没有专用 mark-bad 命令。普通 OOB PROGRAM 保留 main 与 LDPC，main
PROGRAM 保留 OOB head/tail。NAND Core 的 `MEMSETBADBLOCK` 路径会在 BBM/BBT
更新前先擦除 block：four-plane 验收因此检查四个 BBM、BBT 重扫和擦除态，而
不把 markbad 后 main 保持作为契约。

原始介质跨重启持久性验收命令为：

```sh
./scripts/q3n-persistence-smoke.sh
```

脚本执行两轮 guest，验证 NAND Core 标坏产生的 OOB byte 0、标坏时擦除后的
main 摘要和 block-isbad 状态跨重启保留，并验证坏块写擦拒绝。identity 路径
不链接 Page RAID 对象；可选 Page RAID 模式会条件链接同步
`qemu_3dnand_page_raid.o`。NAND-Core 迁移前的异步 scheduler/debugfs ABI
已移除。

read retry 由 NAND Core 调度，共 4 个模式。QEMU 模型按模式提供
`0/8/16/24` bit 的纠错增益；普通读更新 ECC 结果，raw read 返回原始受损数据
且不改变最近一次 single-page scalar/global ECC 结果；multi-plane raw read
则清零其 per-plane ECC array，但不清 scalar/global 最近结果。自动化在 host
模型和驱动 callback 契约层覆盖
40、41、49、57、65 bit 边界。

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

Q3N NAND Core 的 guest 持久化与坏块验收可直接运行：

```sh
./scripts/q3n-persistence-smoke.sh
```

该 host wrapper 使用 fresh NAND 启动第一轮 guest，完成擦除、page/OOB
编程和 NAND Core 标坏；第二轮复用介质并验证 BBT、BBM、主区摘要以及坏块
写擦拒绝。基础结构、补丁、地址换算、ID 白名单、控制器、ECC/read retry
契约可运行 `./scripts/smoke-test.sh` 验证。

four-plane 的一次 guest 验收和两次启动的持久化验收分别为：

```sh
./scripts/shell.sh ./scripts/configure-kernel.sh --q3n-mode multiplane
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/q3n-multiplane-smoke.sh
./scripts/q3n-multiplane-persistence-smoke.sh
```

五种 Linux 存储配置的独立构建契约可运行
`./scripts/q3n-kernel-matrix.sh`。旧 Page RAID 的串行 guest 冒烟仍使用
`./scripts/q3n-serial-smoke.sh`，但它是 NAND-Core 迁移前的 scheduler/debugfs
验收，保留作历史工具而不适用于当前驱动。当前 Page RAID 4:1 回归使用
`./scripts/q3n-page-raid-smoke.sh`；它选择 page-raid mode 和专用
`q3n-page-raid4-smoke.raw`，不会改动 `q3n-nand.raw`。

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
