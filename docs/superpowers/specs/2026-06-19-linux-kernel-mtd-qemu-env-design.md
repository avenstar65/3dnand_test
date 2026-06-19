# Linux 内核 MTD QEMU 开发环境设计

日期：2026-06-19

## 目标

创建一个可复现的开发工作区，用于编译和调试 Linux 内核态软件，覆盖以下能力：

- 默认从官方 kernel.org 源构建最新稳定版 Linux 内核。
- 编译并加载树外内核模块。
- 开发和测试面向 MTD 子系统的内核驱动。
- 通过 QEMU 运行内核和测试软件。
- 使用 GDB 调试内核和模块。

该环境需要同时适用于 macOS 和 Linux。Docker 作为主要的可移植层，宿主机只保留少量必要依赖，构建和运行行为尽量保持一致。

## 当前版本策略

脚本不能把“最新版本”硬编码成某个会过期的内核版本。默认获取内核源码时，应查询官方 kernel.org 元数据或官方镜像，并选择当前 stable 版本；如果调用者显式指定版本，则使用指定版本。

截至 2026-06-19，kernel.org 首页显示：

- mainline：7.1
- stable：7.0.12

这些值只作为本文档日期下的参考。实际实现应在运行时解析当前版本。

## 总体方案

仓库内提供一套本地工具链，包含：

- 用于构建和调试的 Docker 镜像。
- 用于获取源码、配置、编译、运行和调试的小型 shell 脚本。
- 用于开启 QEMU 启动、调试符号、模块加载和 MTD 功能的内核配置片段。
- 一个用于快速启动和稳定测试的最小 initramfs 根文件系统。
- 一个小型 MTD 相关内核模块样例，用来验证树外驱动开发流程。

该方案优先保证可复现性，而不是依赖宿主机上的零散安装步骤。macOS 和 Linux 用户只要具备 Docker，就应能运行同一套脚本。

## 仓库结构

计划目录结构如下：

```text
.
|-- Dockerfile
|-- README.md
|-- configs/
|   |-- linux/
|   |   |-- qemu-x86_64-debug.fragment
|   |   `-- mtd.fragment
|   `-- qemu/
|       `-- x86_64.env
|-- docs/
|   `-- superpowers/specs/
|-- drivers/
|   `-- mtd_demo/
|       |-- Makefile
|       `-- mtd_demo.c
|-- rootfs/
|   |-- init
|   `-- profile.d/
|       `-- mtd.sh
`-- scripts/
    |-- build-image.sh
    |-- shell.sh
    |-- fetch-linux.sh
    |-- configure-kernel.sh
    |-- build-kernel.sh
    |-- build-rootfs.sh
    |-- build-module.sh
    |-- run-qemu.sh
    `-- gdb-kernel.sh
```

构建产物应尽量放在源码管理之外：

```text
work/
|-- linux/
|-- build/
|-- rootfs/
`-- downloads/
```

`work/` 目录应加入 Git 忽略列表。

## 组件设计

### Docker 镜像

Docker 镜像提供编译和调试所需工具：

- 编译工具：`gcc`、`g++`、`make`、`binutils`、`bc`、`bison`、`flex`、`perl`、`python3`、`rsync`。
- 内核依赖：`libelf-dev`、`libssl-dev`、`libncurses-dev`、`dwarves`、`cpio`、`xz-utils`、`zstd`、`git`、`curl`、`ca-certificates`。
- 运行和调试工具：`qemu-system-x86`、`gdb`、`gdb-multiarch`、`strace`。
- MTD 工具：`mtd-utils`，包括可用的 UBI/UBIFS 相关工具。
- BusyBox，或用于把 BusyBox 安装进 initramfs 的工具。

容器会把仓库挂载到稳定路径，例如 `/workspace`。脚本应既能通过宿主机包装命令调用，也能在容器内部直接调用。

### 内核源码获取

`scripts/fetch-linux.sh` 负责：

- 默认从 kernel.org 元数据获取当前 stable 内核。
- 接受显式输入，例如 `--version 7.0.12`、`--mainline` 或 `--longterm`。
- 在可用时下载发布包以及签名或校验信息。
- 复用已有下载文件，除非用户要求强制重新下载。
- 解压到 `work/linux/<version>/`。

这样可以把依赖网络的源码获取步骤和可重复的本地构建步骤分开。

### 内核配置和编译

`scripts/configure-kernel.sh` 和 `scripts/build-kernel.sh` 负责：

- 默认使用 `x86_64` 作为 QEMU 目标架构。
- 从 `x86_64_defconfig` 开始生成配置。
- 合并仓库内的配置片段。
- 开启便于调试的选项，例如调试符号、GDB 脚本、kallsyms、模块卸载和动态调试。
- 开启 MTD core、MTD block、`mtdram`、`nandsim`、UBI 和 UBIFS。
- 在 `work/build/` 下生成 `bzImage`、`vmlinux`、模块和模块安装树。

默认配置优先考虑开发反馈速度和可调试性，而不是最小启动时间或生产环境加固。

### 根文件系统

`scripts/build-rootfs.sh` 创建 initramfs，包含：

- BusyBox 工具。
- 作为启动入口的 `/init`。
- `/proc`、`/sys`、`/dev` 和 `/tmp` 初始化。
- 模块加载支持。
- 用于测试 `mtdram`、`nandsim`、UBI 和 UBIFS 的 MTD 辅助命令。
- 自动测试失败后的 shell 兜底入口。

第一版优先使用 initramfs，因为它启动快，也不需要维护额外磁盘镜像。如果后续需要持久化状态，再增加磁盘镜像支持。

### QEMU 运行

`scripts/run-qemu.sh` 负责：

- 使用生成的 initramfs 启动已编译的 `bzImage`。
- 通过串口控制台在终端输出日志。
- 传入适合调试的内核命令行参数，例如 `console=ttyS0`、`nokaslr` 和 panic 行为。
- 支持普通启动和调试启动。
- 调试模式下添加 QEMU GDB stub 参数 `-s -S`，让内核在执行前等待 GDB 连接。

默认 machine 使用 x86_64 PC，因为它通用性好，内核构建路径也直接。ARM 或板级 Flash 仿真可以通过后续 profile 增加。

### GDB 调试流程

`scripts/gdb-kernel.sh` 负责：

- 使用编译得到的 `vmlinux` 启动 GDB。
- 连接 QEMU 的 `localhost:1234` 调试端口。
- 在内核构建产物提供 helper 脚本时加载这些脚本。
- 给开发者提供一个简单入口，用于在 MTD 核心路径或样例模块中设置断点。

模块调试文档需要说明如何查找模块加载地址，以及如何给树外模块添加符号。

### MTD 驱动开发

默认 MTD 工作流分为两层。

第一层：模拟 MTD 设备。

- 使用 `mtdram` 创建基于 RAM 的 MTD 设备。
- 使用 `nandsim` 模拟 NAND 行为。
- 使用 `mtd-utils`、UBI attach、UBIFS mount、读写和擦除操作进行测试。
- 把这一层作为默认 smoke test，因为它不依赖 QEMU machine 暴露物理 Flash。

第二层：QEMU 板级 Flash profile。

- 在基础环境稳定后增加 `qemu-flash` profile。
- 支持适合 NOR 或 NAND 仿真的 machine/device 组合。
- 将板级行为隔离到 profile 文件里，避免混入默认 x86_64 流程。

这个顺序可以让 MTD 栈和模块逻辑立即进入开发测试，同时保留迁移到更接近硬件仿真的路径。

### 样例模块

`drivers/mtd_demo/` 包含一个小型树外模块，用于：

- 针对已编译的内核树进行构建。
- 打印 init/exit 状态。
- 以保守方式使用 MTD API，验证头文件、符号访问、模块加载和调试流程。
- 避免伪装成完整的生产级 Flash 驱动。

该模块是驱动开发脚手架，也是构建和调试流程的 smoke test。

## 数据流

典型流程如下：

1. 构建 Docker 镜像。
2. 进入容器 shell。
3. 获取选定的 Linux 内核版本。
4. 配置并编译内核。
5. 构建 initramfs。
6. 编译样例模块。
7. 启动 QEMU。
8. 在 guest 内运行 MTD smoke 命令。
9. 需要断点时，用调试模式重新启动 QEMU。
10. 连接 GDB，检查内核或模块行为。

## 错误处理

脚本遇到以下情况应快速失败，并输出清晰错误信息：

- Docker 缺失或不可用。
- 必要的网络下载失败。
- 找不到用户指定的内核版本。
- 无法合并内核配置片段。
- 缺少 `bzImage`、`vmlinux` 或 initramfs 等构建产物。
- QEMU 无法启动，或 GDB 端口已被占用。

在可能的情况下，脚本应打印修复问题后可重试的精确命令。

## 测试和验证

第一版实现应提供以下验证命令：

- Docker 镜像可以成功构建。
- 内核源码可以成功获取。
- 内核可以编译，并生成 `bzImage` 和 `vmlinux`。
- initramfs 可以构建，并包含 `/init`。
- QEMU 可以启动到 shell 或输出脚本化成功标记。
- 可以创建并列出模拟 MTD 设备。
- 可以在模拟 MTD 设备上运行 UBI/UBIFS smoke 步骤。
- 样例模块可以编译并加载。
- 调试模式可以接受 GDB 连接。

第一版不要求所有验证都完全自动化，但每一项都应有明确命令和预期结果。

## 非目标

第一版不做以下事情：

- 提供生产可用的 MTD 硬件驱动。
- 支持所有 CPU 架构。
- 保证精确模拟某个真实 NAND 或 NOR 芯片。
- 管理持久化虚拟机磁盘镜像。
- 替代具体板卡 BSP 集成。

## 后续扩展点

后续可以增加：

- ARM64 QEMU profile。
- 持久化磁盘或根文件系统镜像。
- 板级 Flash profile。
- 用于内核和模块 smoke build 的 CI。
- 除 QEMU GDB stub 之外的串口或网络 `kgdb`。
- 更接近真实 NAND 的坏块和 ECC 测试。
