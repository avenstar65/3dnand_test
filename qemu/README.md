# QEMU q3n-nand Overlay

This directory contains the QEMU source overlay for a basic 3D NAND flash
device and controller model. QEMU intentionally does not implement page-raid,
parity-log layout, parity recovery, or MTD-visible policy. Those belong to the
Linux `qemu_3dnand` driver so the same simulated flash can be used to validate
different kernel-side mapping schemes.

The repository can fetch and build QEMU 11.x automatically. To apply this
overlay manually, copy the files into a QEMU source tree:

```text
qemu/include/hw/mtd/q3n-nand.h -> include/hw/mtd/q3n-nand.h
qemu/include/hw/mtd/q3n-media.h -> include/hw/mtd/q3n-media.h
qemu/hw/mtd/q3n-nand.c         -> hw/block/q3n-nand.c on QEMU 11.x
qemu/hw/mtd/q3n-media.c        -> hw/block/q3n-media.c on QEMU 11.x
qemu/hw/mtd/q3n-media-overlay.h -> hw/block/q3n-media-overlay.h on QEMU 11.x
qemu/hw/mtd/meson.build        -> merge the listed line into hw/block/meson.build
qemu/hw/mtd/Kconfig            -> merge CONFIG_Q3N_NAND into hw/block/Kconfig
```

Implemented base functions:

| Area | Status |
| --- | --- |
| SysBus MMIO/IRQ device skeleton | Implemented |
| PCI BAR0 wrapper for x86_64 discovery | Implemented as `q3n-nand-pci` |
| 2 die x 4 plane geometry constants | Implemented |
| 208/32/3/4 scheme D block-pool defaults | Implemented |
| Versioned sparse physical NAND image | Implemented (`Q3NMEDIA`, v2) |
| Raw NAND byte/overlay restart persistence | Implemented through `BlockBackend` |
| First-page `OOB[0]` bad-block marker | Implemented (`0xff` good, `0x00` bad) |
| 25MiB data block erase | Implemented |
| Basic page read/program/block erase commands | Implemented |
| Independent 128 B logical-OOB read/program commands | Implemented |
| Arbitrary/repeated page program with NAND bytewise-AND semantics | Implemented |
| MMIO data-loss fault injection | Implemented |
| Basic media statistics | Implemented |
| Page-raid/parity append/recovery | Not implemented in QEMU |
| Parity log GC | Not implemented in QEMU |
| Page-RAID runtime-state replay | Not implemented in this phase |
| Linux PCI probe driver | Implemented |
| Linux MTD registration | Implemented in the Linux overlay |
| Linux driver-owned scheme D page-raid | Implemented in the Linux overlay |
| Linux raw NAND `exec_op()` integration | Not implemented |
| Machine/DT wiring | PCI path used first; DT path not implemented |

The MMIO interface is intentionally simple for the first bring-up:

| Register | Offset | Description |
| --- | ---: | --- |
| `Q3N_REG_ID` | `0x0000` | Returns `Q3N1` |
| `Q3N_REG_STATUS` | `0x000c` | Ready/error status |
| `Q3N_REG_CMD` | `0x0010` | Execute base, main-page, or logical-OOB commands |
| `Q3N_REG_ADDR_LO/HI` | `0x0014/0x0018` | Physical byte address in the simulated media |
| `Q3N_REG_LEN` | `0x001c` | Resets PIO buffer for a transfer |
| `Q3N_REG_GEOM0/1` | `0x0020/0x0024` | 16 KiB page/128 B logical OOB and pages/block geometry |
| `Q3N_REG_POOL0/1` | `0x0028/0x002c` | Data/parity/meta/reserve pool sizes |
| `Q3N_REG_OOB_LEN` | `0x0030` | Logical-OOB transfer length (must be 128; resets PIO staging) |
| `Q3N_REG_STAT_*` | `0x0040..0x005c` | Page program/block erase/read-error/fault counters |
| `Q3N_REG_FAULT_ADDR_LO/HI` | `0x0060/0x0064` | Physical byte address for fault injection |
| `Q3N_REG_FAULT_CTRL` | `0x0068` | Trigger data loss, next-program failure, or persistent bitflip injection |
| `Q3N_REG_BLOCK_STATUS` | `0x0080` | Selected physical block bad-block-marker status |
| `Q3N_REG_ECC_GEOM0/1` | `0x0088/0x008c` | 1024 B/40 bit ECC and 96 B/16 step LDPC profile |
| `Q3N_REG_ECC_STATUS..FAILED_STEP` | `0x0090..0x009c` | Latched result of the latest page read |
| `Q3N_REG_FAULT_STEP..REGION` | `0x00a0..0x00ac` | Step, first bit, count, and main/LDPC region for bitflip injection |
| `Q3N_REG_STAT_LDPC_*` | `0x00b0..0x00b8` | Corrected bits, uncorrectable pages, and failed steps |
| `Q3N_REG_DATA` | `0x1000` | PIO data window |

The model exposes a flat physical flash address space:

```text
physical byte address -> physical block -> page
```

Programs may target any physical page, including an already programmed page.
Each successful program stores the bytewise AND of the existing main/OOB bytes
and the incoming bytes, matching NAND's one-way `1` to `0` programming rule.

## Persistent physical media

The PCI wrapper requires a writable BlockBackend. `scripts/run-qemu.sh` connects
the default sparse raw image as `q3n-nand-pci,drive=q3n-media`; `--fresh-nand`
removes that image before QEMU starts and `--nand-image` selects another path.

Image v2 begins with a 4 KiB `Q3NMEDIA` header followed by fixed physical-page
slots. Each physical page is exactly `0x4680` bytes:

```text
0x0000..0x3fff main
0x4000         OOB head / BBM / logical OOB[0]
0x4001..0x4600 LDPC
0x4601..0x467f OOB tail / logical OOB[1..127]
```

The first physical page of every block reserves `0x4000` as the
Linux-compatible bad-block marker. Linux `_block_markbad` constructs a 128 B
logical OOB buffer with byte 0 cleared and submits the ordinary OOB PROGRAM
command; there is no dedicated mark-bad command.

The page slots are followed by sparse, fixed-size error-overlay slots. Each page
has a 16384 B main bitmap and a 1536 B LDPC bitmap. Fault injection XORs bits in
these persistent overlays, so injecting the same range twice restores it;
erasing a block clears all of its overlays. Version 1 images are intentionally
rejected because their physical-page stride and OOB semantics are incompatible
with version 2.

The controller exposes only a 128 B logical OOB: byte 0 maps to `0x4000`, and
bytes 1..127 map to `0x4601..0x467f`. Commands 6 and 7 transfer exactly those
128 bytes and never transfer main data. OOB PROGRAM applies NAND bytewise-AND
semantics while preserving main and LDPC. Main PROGRAM independently updates
the 16 KiB main and deterministically generates all 16 simulated LDPC steps
from the physical page key, profile, step, and 1024 B main-data step while
preserving both logical-OOB ranges. Physical LDPC bytes are never reachable
through the PIO OOB window.

Reads combine the persistent main and LDPC overlay popcounts per step. Up to 40
flipped bits are corrected and return the original main data; 41 or more bits,
or an LDPC value that does not match the stored main, latch an uncorrectable
result and failed-step index. Erased pages use implicit all-`0xff` data and do
not require generated LDPC, but their overlays use the same thresholds.

This format contains no stripe, parity, generation, MTD, UBI, or FTL semantics.
The current durability contract covers normal QEMU shutdown; crash/kill
recovery is not claimed.

QEMU persists raw NAND bytes and bitflip overlays.  The driver does not
restore Page-RAID runtime state after reload or VM restart in this phase.

For x86_64 bring-up, use the PCI wrapper:

```sh
work/build/qemu-11.0.2/qemu-system-x86_64-unsigned -machine q35 -device q3n-nand-pci ...
```

The Linux overlay currently registers an MTD device named `qemu-3dnand`. Its
first read/write/erase path talks to the QEMU model through the controller MMIO
commands. The Linux driver maps MTD logical pages to the QEMU physical media and
owns the current scheme D page-raid/parity-log policy. A raw NAND `exec_op()`
controller integration remains a later phase if we want the Linux raw NAND core
to perform NAND scan and command sequencing itself.

In the guest, `mtd.sh q3n-stats` reads debugfs counters and
`mtd.sh q3n-inject-loss <logical-byte-address>` injects a single data page loss
through the QEMU fault registers. QEMU only reports the physical read failure;
the Linux driver decides whether the missing page can be rebuilt from its
driver-owned page-raid metadata.
