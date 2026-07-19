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
| Normal QEMU restart persistence | Implemented through `BlockBackend` |
| First-page `OOB[0]` bad-block marker | Implemented (`0xff` good, `0x00` bad) |
| 25MiB data block erase | Implemented |
| Basic page read/program/block erase commands | Implemented |
| Combined main+OOB page read/program commands | Implemented |
| Strict ascending page program order per block | Implemented |
| MMIO data-loss fault injection | Implemented |
| Basic media statistics | Implemented |
| Page-raid/parity append/recovery | Not implemented in QEMU |
| Parity log GC | Not implemented in QEMU |
| Physical frontier/status replay ABI | Implemented; Linux rebuilds RAID state |
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
| `Q3N_REG_CMD` | `0x0010` | Execute base commands and combined main+OOB page commands |
| `Q3N_REG_ADDR_LO/HI` | `0x0014/0x0018` | Physical byte address in the simulated media |
| `Q3N_REG_LEN` | `0x001c` | Resets PIO buffer for a transfer |
| `Q3N_REG_GEOM0/1` | `0x0020/0x0024` | Page/OOB and pages/block geometry |
| `Q3N_REG_POOL0/1` | `0x0028/0x002c` | Data/parity/meta/reserve pool sizes |
| `Q3N_REG_OOB_LEN` | `0x0030` | OOB bytes transferred by a combined page command |
| `Q3N_REG_STAT_*` | `0x0040..0x005c` | Page program/block erase/read-error/fault counters |
| `Q3N_REG_FAULT_ADDR_LO/HI` | `0x0060/0x0064` | Physical byte address for fault injection |
| `Q3N_REG_FAULT_CTRL` | `0x0068` | Write `Q3N_FAULT_INJECT_DATA_LOSS` to drop one stored data page |
| `Q3N_REG_STAT_ORDER_ERRORS` | `0x007c` | Rejected out-of-order page programs |
| `Q3N_REG_BLOCK_STATUS` | `0x0080` | Selected physical block bad/erased state |
| `Q3N_REG_BLOCK_NEXT_PAGE` | `0x0084` | Selected block's persistent program frontier |
| `Q3N_REG_DATA` | `0x1000` | PIO data window |

The model exposes a flat physical flash address space:

```text
physical byte address -> physical block -> page
```

After erase, each block accepts page 0 first and advances `next_prog_page` only
after a successful program. Programs that skip or move backward fail without
advancing the block state.

## Persistent physical media

The PCI wrapper requires a writable BlockBackend. `scripts/run-qemu.sh` connects
the default sparse raw image as `q3n-nand-pci,drive=q3n-media`; `--fresh-nand`
removes that image before QEMU starts and `--nand-image` selects another path.

Image v2 begins with a 4 KiB `Q3NMEDIA` header followed by fixed block-state and
page-state arrays. Each physical page has a deterministic 16 KiB main + 1664 B
physical OOB slot: byte 0 is the bad-block marker, bytes 1..1536 hold 16 LDPC
steps of 96 B each, and bytes 1537..1663 hold metadata. Erased page slots remain
sparse holes. The first physical page of every block reserves OOB byte 0 as the
Linux-compatible bad-block marker. A dedicated mark-bad command
programs only that byte and does not advance `next_prog_page`.

The page slots are followed by sparse, fixed-size error-overlay slots. Each page
has a 16384 B main bitmap and a 1536 B LDPC bitmap. Fault injection XORs bits in
these persistent overlays, so injecting the same range twice restores it;
erasing a block clears all of its overlays. Version 1 images are intentionally
rejected because their physical-page stride and OOB semantics are incompatible
with version 2.

This format contains no stripe, parity, generation, MTD, UBI, or FTL semantics.
QEMU persists physical NAND facts only. On probe, the Linux driver queries each
physical data block, rebuilds its serial Page-RAID index, and completes a tail
whose frontier is exactly `D0..D6` before registering the MTD device. The current
durability contract covers normal QEMU shutdown; crash/kill recovery is not
claimed.

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
