# Linux Kernel MTD QEMU Development Environment Design

Date: 2026-06-19

## Goal

Create a reproducible development workspace for compiling and debugging Linux kernel-space software, including:

- Building the latest Linux kernel from official kernel.org sources by default.
- Building and loading out-of-tree kernel modules.
- Developing and testing MTD-oriented kernel drivers.
- Running the kernel and test software under QEMU.
- Debugging the kernel and modules with GDB.

The environment must work from macOS and Linux. Docker is the primary portability layer, so host setup stays small and the build/runtime behavior is consistent.

## Current Version Policy

Scripts must not hard-code a stale "latest" kernel release. The default kernel fetch flow will query official kernel.org metadata or mirrors and select the current stable release unless the caller provides an explicit version.

As of 2026-06-19, the kernel.org homepage shows:

- mainline: 7.1
- stable: 7.0.12

These values are only a reference for this design date. The implementation should resolve the current version at runtime.

## Approach

Use a repository-local toolchain made of:

- A Docker image for build and debug tools.
- Small shell scripts for fetching, configuring, building, running, and debugging.
- Kernel config fragments that enable QEMU boot, debug symbols, module loading, and MTD features.
- A minimal initramfs root filesystem for fast boot and predictable tests.
- A small sample MTD-oriented kernel module for validating the out-of-tree driver workflow.

This favors repeatability over host-specific installation instructions. Users on macOS or Linux should be able to run the same scripts once Docker is available.

## Repository Layout

Planned layout:

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

Generated artifacts will live outside source-controlled project files where practical:

```text
work/
|-- linux/
|-- build/
|-- rootfs/
`-- downloads/
```

The `work/` directory should be ignored by Git.

## Components

### Docker Image

The Docker image will provide the tools needed to build and debug:

- Compiler and build tools: `gcc`, `g++`, `make`, `binutils`, `bc`, `bison`, `flex`, `perl`, `python3`, `rsync`.
- Kernel dependencies: `libelf-dev`, `libssl-dev`, `libncurses-dev`, `dwarves`, `cpio`, `xz-utils`, `zstd`, `git`, `curl`, `ca-certificates`.
- Runtime/debug tools: `qemu-system-x86`, `gdb`, `gdb-multiarch`, `strace`.
- MTD tools: `mtd-utils`, including UBI/UBIFS utilities where available.
- BusyBox or tools to build/install BusyBox into the initramfs.

The container will mount the repository into a stable path such as `/workspace`. Scripts will be written so they can be called from the host wrapper or inside the container.

### Kernel Fetching

`scripts/fetch-linux.sh` will:

- Default to the current stable kernel from kernel.org metadata.
- Accept explicit inputs such as `--version 7.0.12`, `--mainline`, or `--longterm`.
- Download release tarballs and signatures/checksums when available.
- Reuse existing downloads unless forced.
- Extract into `work/linux/<version>/`.

The design keeps network-dependent source fetching separate from deterministic local builds.

### Kernel Configuration and Build

`scripts/configure-kernel.sh` and `scripts/build-kernel.sh` will:

- Use `x86_64` as the default QEMU target.
- Start from `x86_64_defconfig`.
- Merge repository config fragments.
- Enable debug-friendly options such as debug symbols, GDB scripts, kallsyms, module unloading, and dynamic debug.
- Enable MTD core, MTD block devices, `mtdram`, `nandsim`, UBI, and UBIFS.
- Produce `bzImage`, `vmlinux`, modules, and a module install tree under `work/build/`.

The default config should optimize for developer feedback and debuggability rather than minimal boot time or production hardening.

### Root Filesystem

`scripts/build-rootfs.sh` will create an initramfs with:

- BusyBox utilities.
- `/init` as the boot entry point.
- `/proc`, `/sys`, `/dev`, and `/tmp` setup.
- Module loading support.
- MTD helper commands for smoke testing `mtdram`, `nandsim`, UBI, and UBIFS.
- A shell fallback if automated tests fail.

The initramfs is preferred for the first version because it boots quickly and avoids separate disk image management. Disk image support can be added later if persistent state becomes important.

### QEMU Runtime

`scripts/run-qemu.sh` will:

- Boot the built `bzImage` with the generated initramfs.
- Use serial console output on the terminal.
- Pass kernel command-line arguments suitable for debugging, including `console=ttyS0`, `nokaslr`, and panic behavior.
- Support normal boot and debug boot.
- In debug mode, add the QEMU GDB stub with `-s -S` so the kernel waits for GDB before execution.

The default machine will be x86_64 PC because it is broadly available and has a straightforward kernel build path. Additional profiles can be introduced for ARM or board-specific Flash emulation.

### GDB Workflow

`scripts/gdb-kernel.sh` will:

- Start GDB against the built `vmlinux`.
- Connect to QEMU on `localhost:1234`.
- Load kernel helper scripts when the kernel build provides them.
- Provide a simple place for developer commands such as breakpoints in core MTD paths or the sample module.

Module debugging will document how to find module load addresses and add symbols for out-of-tree modules.

### MTD Driver Development

The default MTD workflow has two levels.

Level 1: simulated MTD devices:

- Use `mtdram` to create RAM-backed MTD devices.
- Use `nandsim` for NAND-like behavior.
- Test with `mtd-utils`, UBI attach, UBIFS mount, read/write, and erase operations.
- Use this level as the default smoke test because it does not depend on a QEMU board exposing physical Flash.

Level 2: QEMU board Flash profile:

- Add a `qemu-flash` profile after the base environment is reliable.
- Support a machine/device combination suitable for NOR or NAND emulation.
- Keep board-specific behavior isolated in profile files instead of mixing it into the default x86_64 flow.

This sequence lets MTD stack and module logic be developed immediately, while leaving a clean path toward hardware-like simulation.

### Sample Module

`drivers/mtd_demo/` will contain a small out-of-tree module that:

- Builds against the compiled kernel tree.
- Logs init/exit state.
- Uses MTD APIs conservatively enough to validate headers, symbol access, module loading, and debugging.
- Avoids pretending to be a full production Flash driver.

The module is a scaffold for driver development and a smoke test for the build/debug workflow.

## Data Flow

Typical flow:

1. Build the Docker image.
2. Enter the container shell.
3. Fetch the selected Linux kernel release.
4. Configure and build the kernel.
5. Build the initramfs.
6. Build the sample module.
7. Boot QEMU.
8. Run MTD smoke commands inside the guest.
9. Relaunch QEMU in debug mode when breakpoints are needed.
10. Attach GDB to inspect kernel or module behavior.

## Error Handling

Scripts should fail fast with clear messages when:

- Docker is missing or unavailable.
- Required network downloads fail.
- A requested kernel version cannot be found.
- Kernel config fragments cannot be merged.
- Build artifacts such as `bzImage`, `vmlinux`, or initramfs are missing.
- QEMU cannot start or the GDB port is already in use.

Where possible, scripts should print the exact next command to retry after fixing the problem.

## Testing and Verification

The first implementation should provide verification commands for:

- Docker image builds successfully.
- Kernel source fetch succeeds.
- Kernel compiles and produces `bzImage` and `vmlinux`.
- Initramfs builds and contains `/init`.
- QEMU boots to a shell or scripted success marker.
- MTD simulated devices can be created and listed.
- UBI/UBIFS smoke steps can run against a simulated MTD device.
- Sample module builds and loads.
- Debug mode accepts a GDB connection.

Not every verification must be fully automated in the first pass, but each must have a documented command and expected result.

## Non-Goals

The first version will not:

- Provide a production-ready MTD hardware driver.
- Support every architecture.
- Guarantee exact behavior of a specific physical NAND or NOR chip.
- Manage persistent VM disk images.
- Replace board-specific BSP integration.

## Open Extension Points

Future work can add:

- ARM64 QEMU profile.
- Persistent disk/rootfs images.
- Board-specific Flash profiles.
- CI for kernel/module smoke builds.
- `kgdb` over serial or network in addition to QEMU's GDB stub.
- More realistic bad-block and ECC testing for NAND-oriented development.
