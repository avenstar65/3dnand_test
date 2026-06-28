# QEMU q3n-nand Overlay

This directory contains a QEMU source overlay for the 3D NAND scheme D base
model. The repository currently runs the host-installed `qemu-system-x86_64`;
it does not vendor a full QEMU source tree. To build this model, copy the
overlay files into a QEMU source tree:

```text
qemu/include/hw/mtd/q3n-nand.h -> include/hw/mtd/q3n-nand.h
qemu/hw/mtd/q3n-nand.c         -> hw/block/q3n-nand.c on QEMU 11.x
qemu/hw/mtd/meson.build        -> merge the listed line into hw/block/meson.build
qemu/hw/mtd/Kconfig            -> merge CONFIG_Q3N_NAND into hw/block/Kconfig
```

Implemented base functions:

| Area | Status |
| --- | --- |
| SysBus MMIO/IRQ device skeleton | Implemented |
| 2 die x 4 plane geometry constants | Implemented |
| 208/32/3/4 scheme D block-pool defaults | Implemented |
| Sparse 16KiB page media | Implemented |
| 25MiB data block erase | Implemented |
| Data block generation | Implemented |
| 8-lane XOR parity append | Implemented |
| In-memory latest parity index | Implemented |
| Basic single-page recovery hook | Implemented |
| Parity log GC | Not implemented |
| Checkpoint/replay | Not implemented |
| Linux raw NAND driver | Not implemented |
| Machine/DT wiring | Not implemented |

The MMIO interface is intentionally simple for the first bring-up:

| Register | Offset | Description |
| --- | ---: | --- |
| `Q3N_REG_ID` | `0x0000` | Returns `Q3N1` |
| `Q3N_REG_STATUS` | `0x000c` | Ready/error/recovered/parity status |
| `Q3N_REG_CMD` | `0x0010` | Execute `READ_ID`, `READ_PAGE`, `PROGRAM_PAGE`, `ERASE_BLOCK`, `RESET` |
| `Q3N_REG_ADDR_LO/HI` | `0x0014/0x0018` | Logical byte address |
| `Q3N_REG_LEN` | `0x001c` | Resets PIO buffer for a transfer |
| `Q3N_REG_POOL0/1` | `0x0028/0x002c` | Data/parity/meta/reserve pool sizes |
| `Q3N_REG_DATA` | `0x1000` | PIO data window |

The model exposes a controller-private logical address space:

```text
logical byte address -> data block -> lane/block/page
```

Linux MTD/raw NAND integration still needs a controller driver and QEMU
machine/firmware wiring. Until those are added, this overlay is a base QEMU
implementation unit rather than a runnable device in the repository's default
`scripts/run-qemu.sh` flow.
