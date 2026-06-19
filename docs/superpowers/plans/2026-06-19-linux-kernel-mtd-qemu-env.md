# Linux Kernel MTD QEMU Environment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建一个基于 Docker、QEMU、GDB 和 MTD 仿真的 Linux 内核态开发环境。

**Architecture:** 仓库提供宿主机入口脚本、容器内构建脚本、内核配置片段、initramfs 文件和样例树外模块。重型构建在 Docker 容器内完成，`work/` 保存下载和构建产物，源码目录只保存可复现配置。

**Tech Stack:** POSIX shell、Docker、Ubuntu、Linux kernel、BusyBox、QEMU、GDB、mtd-utils、kself-style smoke scripts。

---

## 文件结构

- Create: `.gitignore`，忽略 `work/`、日志和临时产物。
- Create: `Dockerfile`，定义内核构建和 QEMU 调试工具镜像。
- Create: `README.md`，中文使用说明。
- Create: `configs/linux/qemu-x86_64-debug.fragment`，QEMU x86_64 调试配置。
- Create: `configs/linux/mtd.fragment`，MTD、UBI、UBIFS、模拟设备配置。
- Create: `configs/qemu/x86_64.env`，QEMU 默认参数。
- Create: `rootfs/init`，guest 启动入口。
- Create: `rootfs/profile.d/mtd.sh`，guest 内 MTD smoke helper。
- Create: `drivers/mtd_demo/Makefile`，树外模块构建入口。
- Create: `drivers/mtd_demo/mtd_demo.c`，样例 MTD 模块。
- Create: `scripts/lib/common.sh`，通用路径和错误处理。
- Create: `scripts/build-image.sh`，构建 Docker 镜像。
- Create: `scripts/shell.sh`，进入或执行容器命令。
- Create: `scripts/fetch-linux.sh`，获取 kernel.org 内核源码。
- Create: `scripts/configure-kernel.sh`，生成内核配置。
- Create: `scripts/build-kernel.sh`，编译内核和模块。
- Create: `scripts/build-rootfs.sh`，生成 initramfs。
- Create: `scripts/build-module.sh`，构建样例模块。
- Create: `scripts/run-qemu.sh`，启动 QEMU。
- Create: `scripts/gdb-kernel.sh`，连接 GDB。
- Create: `scripts/smoke-test.sh`，本地轻量验证。
- Create: `tests/test_scripts.sh`，shell 脚本结构测试。

## Task 1: 添加测试骨架

**Files:**
- Create: `tests/test_scripts.sh`

- [ ] **Step 1: 写失败测试**

```sh
#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_file() {
  [ -f "$repo_root/$1" ] || fail "missing file: $1"
}

assert_executable() {
  [ -x "$repo_root/$1" ] || fail "not executable: $1"
}

assert_contains() {
  file=$1
  pattern=$2
  grep -Eq "$pattern" "$repo_root/$file" || fail "$file does not contain pattern: $pattern"
}

for file in \
  Dockerfile \
  README.md \
  scripts/lib/common.sh \
  scripts/build-image.sh \
  scripts/shell.sh \
  scripts/fetch-linux.sh \
  scripts/configure-kernel.sh \
  scripts/build-kernel.sh \
  scripts/build-rootfs.sh \
  scripts/build-module.sh \
  scripts/run-qemu.sh \
  scripts/gdb-kernel.sh \
  scripts/smoke-test.sh \
  configs/linux/qemu-x86_64-debug.fragment \
  configs/linux/mtd.fragment \
  configs/qemu/x86_64.env \
  rootfs/init \
  rootfs/profile.d/mtd.sh \
  drivers/mtd_demo/Makefile \
  drivers/mtd_demo/mtd_demo.c; do
  assert_file "$file"
done

for file in scripts/build-image.sh scripts/shell.sh scripts/fetch-linux.sh scripts/configure-kernel.sh scripts/build-kernel.sh scripts/build-rootfs.sh scripts/build-module.sh scripts/run-qemu.sh scripts/gdb-kernel.sh scripts/smoke-test.sh rootfs/init rootfs/profile.d/mtd.sh; do
  assert_executable "$file"
done

assert_contains Dockerfile 'qemu-system-x86'
assert_contains Dockerfile 'mtd-utils'
assert_contains scripts/fetch-linux.sh 'kernel.org'
assert_contains scripts/run-qemu.sh '-s -S'
assert_contains configs/linux/mtd.fragment 'CONFIG_MTD_NAND_NANDSIM'
assert_contains drivers/mtd_demo/mtd_demo.c 'mtd_for_each_device'
assert_contains README.md 'QEMU'
assert_contains README.md 'MTD'

printf 'ok: script structure verified\n'
```

- [ ] **Step 2: 运行测试确认失败**

Run: `sh tests/test_scripts.sh`

Expected: FAIL，提示缺少 `Dockerfile` 或其他待创建文件。

## Task 2: 创建核心环境文件

**Files:**
- Create: `.gitignore`
- Create: `Dockerfile`
- Create: `scripts/lib/common.sh`
- Create: `scripts/build-image.sh`
- Create: `scripts/shell.sh`

- [ ] **Step 1: 写最小实现**

创建 Docker 镜像定义、通用函数和宿主机入口脚本。脚本必须使用 `set -eu`，并通过 `repo_root` 锁定工作区路径。

- [ ] **Step 2: 运行结构测试**

Run: `sh tests/test_scripts.sh`

Expected: 仍然 FAIL，因为后续脚本、配置和驱动还不存在。

## Task 3: 创建内核构建脚本和配置

**Files:**
- Create: `scripts/fetch-linux.sh`
- Create: `scripts/configure-kernel.sh`
- Create: `scripts/build-kernel.sh`
- Create: `configs/linux/qemu-x86_64-debug.fragment`
- Create: `configs/linux/mtd.fragment`

- [ ] **Step 1: 实现内核获取、配置和编译脚本**

`fetch-linux.sh` 支持 `--version`、`--mainline`、`--stable`、`--force`。默认解析 kernel.org `releases.json` 的 stable 版本。

- [ ] **Step 2: 运行结构测试**

Run: `sh tests/test_scripts.sh`

Expected: 仍然 FAIL，因为 rootfs、QEMU 和模块文件还不存在。

## Task 4: 创建 rootfs、QEMU 和 GDB 脚本

**Files:**
- Create: `rootfs/init`
- Create: `rootfs/profile.d/mtd.sh`
- Create: `configs/qemu/x86_64.env`
- Create: `scripts/build-rootfs.sh`
- Create: `scripts/run-qemu.sh`
- Create: `scripts/gdb-kernel.sh`

- [ ] **Step 1: 实现 initramfs、QEMU 启动和 GDB 连接**

initramfs 需要复制 BusyBox、模块目录和 rootfs 模板。QEMU 默认串口输出，调试模式添加 `-s -S`。

- [ ] **Step 2: 运行结构测试**

Run: `sh tests/test_scripts.sh`

Expected: 仍然 FAIL，因为样例模块和 README 还不存在。

## Task 5: 创建样例模块和中文 README

**Files:**
- Create: `drivers/mtd_demo/Makefile`
- Create: `drivers/mtd_demo/mtd_demo.c`
- Create: `scripts/build-module.sh`
- Create: `scripts/smoke-test.sh`
- Create: `README.md`

- [ ] **Step 1: 实现样例模块和文档**

模块 init 时遍历 MTD 设备并输出信息；README 用中文说明完整流程。

- [ ] **Step 2: 运行结构测试确认通过**

Run: `sh tests/test_scripts.sh`

Expected: PASS，输出 `ok: script structure verified`。

## Task 6: 验证和提交

**Files:**
- Modify: all created files

- [ ] **Step 1: 运行轻量 smoke 测试**

Run: `./scripts/smoke-test.sh`

Expected: PASS，确认脚本可执行、关键文本存在、shell 语法基本有效。

- [ ] **Step 2: 检查 Git 状态**

Run: `git status --short`

Expected: 只出现本次实现新增或修改的文件。

- [ ] **Step 3: 提交实现**

```bash
git add .
git commit -m "Build Linux kernel MTD QEMU environment"
```

Expected: commit succeeds.

## 自检

- 覆盖 spec 中 Docker、内核获取、内核编译、rootfs、QEMU、GDB、MTD 仿真、样例模块、中文文档和验证命令。
- 无 TBD/TODO/FIXME 占位。
- 第一版不尝试真实完整内核编译验证，避免在没有 Docker/network/QEMU 权限时阻塞；README 明确给出完整命令。
