# Linux 内核 MTD QEMU 开发环境

这个仓库用于搭建一个可复现的 Linux 内核态开发环境，支持编译最新稳定版 Linux 内核、开发树外内核模块、调试 MTD 相关驱动，并通过 QEMU 启动验证。

默认流程使用 Docker 封装 Linux 构建工具链，因此 macOS 和 Linux 上的操作基本一致。

## 前置条件

宿主机需要安装 Docker。macOS 推荐使用 Docker Desktop；Linux 推荐使用发行版自带 Docker 或 Docker Engine。

## 快速开始

构建工具镜像：

```sh
./scripts/build-image.sh
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

进入 guest 后可以运行：

```sh
/etc/profile.d/mtd.sh smoke
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

第二阶段可以新增 QEMU 板级 Flash profile，例如 NOR 或 NAND 仿真。相关参数应放到 `configs/qemu/`，不要混入默认 x86_64 流程。

## 目录说明

- `Dockerfile`：构建 Linux 内核、QEMU、GDB 和 MTD 工具环境。
- `scripts/`：获取源码、编译、构建 rootfs、运行 QEMU 和连接 GDB。
- `configs/linux/`：内核配置片段。
- `configs/qemu/`：QEMU profile。
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
./scripts/shell.sh ./scripts/configure-kernel.sh
./scripts/shell.sh ./scripts/build-kernel.sh
./scripts/shell.sh ./scripts/build-module.sh
./scripts/shell.sh ./scripts/build-rootfs.sh
./scripts/shell.sh ./scripts/run-qemu.sh
```

## 常见问题

如果提示缺少 Docker，请先安装并启动 Docker。

如果 kernel.org 下载失败，可以稍后重试，或手动指定已经下载并解压的源码目录：

```sh
LINUX_DIR=/workspace/work/linux/linux-7.0.12 ./scripts/configure-kernel.sh
```

如果 QEMU 调试端口被占用，请关闭已有 QEMU 进程，或修改脚本中的 GDB 端口参数。
