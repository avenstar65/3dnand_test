/*
 * QEMU 3D NAND model for scheme D experiments.
 *
 * This header is intended to be copied into a QEMU source tree under
 * include/hw/mtd/q3n-nand.h.
 */

#ifndef HW_MTD_Q3N_NAND_H
#define HW_MTD_Q3N_NAND_H

#include "hw/core/irq.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_Q3N_NAND "q3n-nand"
OBJECT_DECLARE_SIMPLE_TYPE(Q3NNandState, Q3N_NAND)

#define TYPE_Q3N_NAND_PCI "q3n-nand-pci"

#define Q3N_PCI_VENDOR_ID              0x1b36
#define Q3N_PCI_DEVICE_ID              0x003d
#define Q3N_PCI_REVISION               0x01

#define Q3N_MMIO_SIZE                 0x10000

#define Q3N_DIES                      2
#define Q3N_PLANES_PER_DIE            4
#define Q3N_LANES                     8
#define Q3N_BLOCKS_PER_PLANE          247
#define Q3N_PAGES_PER_BLOCK           1600
#define Q3N_PAGE_SIZE                 (16 * 1024)
#define Q3N_OOB_SIZE                  1024

#define Q3N_DEFAULT_DATA_BLOCKS_PER_PLANE       208
#define Q3N_DEFAULT_PARITY_BLOCKS_PER_PLANE      32
#define Q3N_DEFAULT_METADATA_BLOCKS_PER_PLANE     3
#define Q3N_DEFAULT_RESERVE_BLOCKS_PER_PLANE      4

enum q3n_reg {
    Q3N_REG_ID                 = 0x0000,
    Q3N_REG_CAP                = 0x0004,
    Q3N_REG_CTRL               = 0x0008,
    Q3N_REG_STATUS             = 0x000c,
    Q3N_REG_CMD                = 0x0010,
    Q3N_REG_ADDR_LO            = 0x0014,
    Q3N_REG_ADDR_HI            = 0x0018,
    Q3N_REG_LEN                = 0x001c,
    Q3N_REG_GEOM0              = 0x0020,
    Q3N_REG_GEOM1              = 0x0024,
    Q3N_REG_POOL0              = 0x0028,
    Q3N_REG_POOL1              = 0x002c,
    Q3N_REG_RAID_PROFILE       = 0x0030,
    Q3N_REG_RAID_STATUS        = 0x0034,
    Q3N_REG_IRQ_STATUS         = 0x0038,
    Q3N_REG_IRQ_MASK           = 0x003c,
    Q3N_REG_STAT_DATA_PROGRAMS = 0x0040,
    Q3N_REG_STAT_DATA_ERASES   = 0x0044,
    Q3N_REG_STAT_PARITY_APPENDS = 0x0048,
    Q3N_REG_STAT_RAID_RECOVERED = 0x004c,
    Q3N_REG_STAT_RAID_FAILED   = 0x0050,
    Q3N_REG_DATA               = 0x1000,
};

enum q3n_cmd {
    Q3N_CMD_NOP        = 0,
    Q3N_CMD_READ_ID    = 1,
    Q3N_CMD_READ_PAGE  = 2,
    Q3N_CMD_PROGRAM_PAGE = 3,
    Q3N_CMD_ERASE_BLOCK = 4,
    Q3N_CMD_RESET      = 5,
};

enum q3n_status {
    Q3N_STATUS_READY          = 1U << 0,
    Q3N_STATUS_ERROR          = 1U << 1,
    Q3N_STATUS_RECOVERED      = 1U << 2,
    Q3N_STATUS_PARITY_VALID   = 1U << 3,
    Q3N_STATUS_PARITY_STALE   = 1U << 4,
};

enum q3n_irq {
    Q3N_IRQ_DONE  = 1U << 0,
    Q3N_IRQ_ERROR = 1U << 1,
};

MemoryRegion *q3n_nand_get_mmio(Q3NNandState *s);
void q3n_nand_set_irq(Q3NNandState *s, qemu_irq irq);

#endif
