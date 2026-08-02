# Task 9 report — overlays and Q3N storage-mode matrix

## RED

- `sh tests/test_scripts.sh` first failed because normal Linux overlay output
  omitted `qemu_3dnand_multiplane_layout.c`; the behavior fixture also
  required the remaining five Task 7 MP files, exact QEMU multi-plane output,
  idempotent checksums, and recorded matrix invocations.
- The strict artifact fixture initially rejected `Q3N_EXPECTED_MODE=raid2`.
- A direct isolated `.ko` target first failed Modpost because no `vmlinux.o`
  existed. A single core ABI seed fixed that prerequisite, but direct
  in-tree Modpost still could not consume provider symbols from a copied or
  external `Module.symvers`. Kernel `scripts/Makefile.modpost` confirms that
  `KBUILD_EXTRA_SYMBOLS` is for external modules; the in-tree path creates
  its own module symbol dump.

## GREEN

- Linux overlay now installs all common driver files plus the six MP layout,
  hardware, and operation source/header files. QEMU overlay installs the
  multi-plane source/header and the exact four-source Meson entry.
- The matrix behavior fixture executes real scripts in fake trees and checks
  copied content, checksums, output Meson composition, five unique build
  roots, recorded configure/module/contract calls, and actual generated
  module artifacts. It does not inspect script text for features.
- Strict contract accepts `raid2` and requires its exact Kconfig ratio `2`.
- `q3n-kernel-matrix.sh` builds one clean identity `vmlinux.o` ABI seed, then
  independently configures every profile and runs `make modules` in each
  profile root. This is the reliable in-tree target that generates a fresh
  `qemu_3dnand.ko` and a coherent provider map without
  `KBUILD_MODPOST_WARN`. Every mode invokes the strict contract with an
  explicit expected mode.

## Verification

- `sh tests/test_scripts.sh`
- `sh tests/test_q3n_nand_core_contract_strict.sh`
- `sh scripts/smoke-test.sh`
- shell syntax checks for all scripts and `git diff --check`
- Real `./scripts/q3n-kernel-matrix.sh` in the project container:
  identity, Page RAID 2:1, Page RAID 4:1, Page RAID 8:1, and multi-plane all
  built fresh Q3N module artifacts and each passed the strict compiled
  contract. The final container exited `0` with `oom=false`.

## Environment note

The desktop command runner terminates long foreground kernel builds. The
real matrix was therefore monitored in a detached Docker container using the
same `/workspace` mount and build image. The isolated build artifacts remain
under ignored `work/build-matrix/`; no generated build output is tracked.
