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
#define Q3N_REG_RAID_PROFILE           0x0030
#define Q3N_REG_DATA                   0x1000

#define Q3N_CAP_SCHEME_D               BIT(0)
#define Q3N_RAID_PROFILE_SCHEME_D      4

#define Q3N_STATUS_READY               BIT(0)
#define Q3N_STATUS_ERROR               BIT(1)

#define Q3N_CMD_READ_PAGE              2
#define Q3N_CMD_PROGRAM_PAGE           3
#define Q3N_CMD_ERASE_BLOCK            4

#endif
