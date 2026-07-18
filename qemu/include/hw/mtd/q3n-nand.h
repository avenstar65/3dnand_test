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

#define Q3N_CAP_BASIC_FLASH             (1U << 0)
#define Q3N_CAP_PERSISTENT_MEDIA        (1U << 1)
#define Q3N_CAP_BAD_BLOCK_MARKER        (1U << 2)

#define Q3N_BLOCK_STATUS_BAD            (1U << 0)
#define Q3N_BLOCK_STATUS_ERASED         (1U << 1)

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
    Q3N_REG_OOB_LEN            = 0x0030,
    Q3N_REG_IRQ_STATUS         = 0x0038,
    Q3N_REG_IRQ_MASK           = 0x003c,
    Q3N_REG_STAT_PAGE_PROGRAMS = 0x0040,
    Q3N_REG_STAT_BLOCK_ERASES  = 0x0044,
    Q3N_REG_STAT_PAGE_READ_ERRORS = 0x0048,
    Q3N_REG_STAT_FAULTS_INJECTED = 0x005c,
    Q3N_REG_FAULT_ADDR_LO      = 0x0060,
    Q3N_REG_FAULT_ADDR_HI      = 0x0064,
    Q3N_REG_FAULT_CTRL         = 0x0068,
    Q3N_REG_STAT_FG_OPS        = 0x0070,
    Q3N_REG_STAT_PARITY_READS  = 0x0074,
    Q3N_REG_STAT_PARITY_WRITES = 0x0078,
    Q3N_REG_STAT_ORDER_ERRORS  = 0x007c,
    Q3N_REG_BLOCK_STATUS       = 0x0080,
    Q3N_REG_BLOCK_NEXT_PAGE    = 0x0084,
    Q3N_REG_DATA               = 0x1000,
};

enum q3n_cmd {
    Q3N_CMD_NOP        = 0,
    Q3N_CMD_READ_ID    = 1,
    Q3N_CMD_READ_PAGE  = 2,
    Q3N_CMD_PROGRAM_PAGE = 3,
    Q3N_CMD_ERASE_BLOCK = 4,
    Q3N_CMD_RESET      = 5,
    Q3N_CMD_READ_PAGE_OOB = 6,
    Q3N_CMD_PROGRAM_PAGE_OOB = 7,
    Q3N_CMD_GET_BLOCK_STATUS = 8,
    Q3N_CMD_MARK_BAD_BLOCK = 9,
};

enum q3n_status {
    Q3N_STATUS_READY          = 1U << 0,
    Q3N_STATUS_ERROR          = 1U << 1,
};

enum q3n_irq {
    Q3N_IRQ_DONE  = 1U << 0,
    Q3N_IRQ_ERROR = 1U << 1,
};

enum q3n_fault {
    Q3N_FAULT_INJECT_DATA_LOSS = 1U << 0,
};

MemoryRegion *q3n_nand_get_mmio(Q3NNandState *s);
void q3n_nand_set_irq(Q3NNandState *s, qemu_irq irq);

#endif
