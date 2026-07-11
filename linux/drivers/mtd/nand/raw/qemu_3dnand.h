/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register definitions shared with the QEMU q3n-nand model.
 */

#ifndef _QEMU_3DNAND_H
#define _QEMU_3DNAND_H

#define Q3N_PCI_VENDOR_ID              0x1b36
#define Q3N_PCI_DEVICE_ID              0x003d

#define Q3N_ID_VALUE                   0x314e3351U

#define Q3N_REG_ID                     0x0000
#define Q3N_REG_CAP                    0x0004
#define Q3N_REG_STATUS                 0x000c
#define Q3N_REG_CMD                    0x0010
#define Q3N_REG_ADDR_LO                0x0014
#define Q3N_REG_ADDR_HI                0x0018
#define Q3N_REG_LEN                    0x001c
#define Q3N_REG_GEOM0                  0x0020
#define Q3N_REG_GEOM1                  0x0024
#define Q3N_REG_POOL0                  0x0028
#define Q3N_REG_POOL1                  0x002c
#define Q3N_REG_OOB_LEN                0x0030
#define Q3N_REG_STAT_PAGE_PROGRAMS     0x0040
#define Q3N_REG_STAT_BLOCK_ERASES      0x0044
#define Q3N_REG_STAT_PAGE_READ_ERRORS  0x0048
#define Q3N_REG_STAT_FAULTS_INJECTED   0x005c
#define Q3N_REG_FAULT_ADDR_LO          0x0060
#define Q3N_REG_FAULT_ADDR_HI          0x0064
#define Q3N_REG_FAULT_CTRL             0x0068
#define Q3N_REG_STAT_FG_OPS            0x0070
#define Q3N_REG_STAT_PARITY_READS      0x0074
#define Q3N_REG_STAT_PARITY_WRITES     0x0078
#define Q3N_REG_STAT_ORDER_ERRORS      0x007c
#define Q3N_REG_DATA                   0x1000

#define Q3N_CAP_SCHEME_D               BIT(0)

#define Q3N_STATUS_READY               BIT(0)
#define Q3N_STATUS_ERROR               BIT(1)

#define Q3N_CMD_READ_PAGE              2
#define Q3N_CMD_PROGRAM_PAGE           3
#define Q3N_CMD_ERASE_BLOCK            4
#define Q3N_CMD_READ_PAGE_OOB          6
#define Q3N_CMD_PROGRAM_PAGE_OOB       7

#define Q3N_FAULT_INJECT_DATA_LOSS     BIT(0)

#endif
