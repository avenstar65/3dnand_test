/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register definitions shared with the QEMU q3n-nand model.
 */

#ifndef _QEMU_3DNAND_H
#define _QEMU_3DNAND_H

#define Q3N_PCI_VENDOR_ID              0x1b36
#define Q3N_PCI_DEVICE_ID              0x003d

#define Q3N_ID_VALUE                   0x314e3351U

#define Q3N_PAGE_SIZE                  (16 * 1024)
#define Q3N_PHYSICAL_OOB_SIZE          1664U
#define Q3N_LOGICAL_OOB_SIZE           128U
#define Q3N_BBM_OOB_OFFSET             0U
#define Q3N_LDPC_OOB_OFFSET            1U
#define Q3N_LDPC_BYTES_PER_STEP        96U
#define Q3N_LDPC_STEPS                 16U
#define Q3N_LDPC_TOTAL_BYTES           1536U
#define Q3N_METADATA_OOB_OFFSET        1537U
#define Q3N_PHYSICAL_OOB_HEAD_OFFSET   Q3N_PAGE_SIZE
#define Q3N_PHYSICAL_OOB_HEAD_SIZE     1U
#define Q3N_PHYSICAL_LDPC_OFFSET       \
	(Q3N_PHYSICAL_OOB_HEAD_OFFSET + Q3N_PHYSICAL_OOB_HEAD_SIZE)
#define Q3N_PHYSICAL_LDPC_SIZE         Q3N_LDPC_TOTAL_BYTES
#define Q3N_PHYSICAL_OOB_TAIL_OFFSET   \
	(Q3N_PHYSICAL_LDPC_OFFSET + Q3N_PHYSICAL_LDPC_SIZE)
#define Q3N_PHYSICAL_OOB_TAIL_SIZE     (Q3N_LOGICAL_OOB_SIZE - 1U)
#define Q3N_PHYSICAL_PAGE_SIZE         \
	(Q3N_PHYSICAL_OOB_TAIL_OFFSET + Q3N_PHYSICAL_OOB_TAIL_SIZE)
#define Q3N_ECC_STEP_SIZE              1024U
#define Q3N_ECC_STRENGTH               40U

#if Q3N_PAGE_SIZE / Q3N_ECC_STEP_SIZE != Q3N_LDPC_STEPS
#error "Q3N page must contain one LDPC step per ECC step"
#endif

#if Q3N_PHYSICAL_OOB_HEAD_OFFSET != 0x4000U
#error "Q3N physical OOB head must follow the main page"
#endif

#if Q3N_PHYSICAL_LDPC_OFFSET != 0x4001U
#error "Q3N physical LDPC must follow the OOB head"
#endif

#if Q3N_PHYSICAL_OOB_TAIL_OFFSET != 0x4601U
#error "Q3N physical OOB tail must follow LDPC"
#endif

#if Q3N_PHYSICAL_PAGE_SIZE != 0x4680U
#error "Q3N physical page size must include main, OOB, and LDPC"
#endif

#if Q3N_LOGICAL_OOB_SIZE != 1U + 127U
#error "Q3N logical OOB layout must be BBM + metadata"
#endif

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
#define Q3N_REG_OP_CLASS               0x0034
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
#define Q3N_REG_BLOCK_STATUS           0x0080
#define Q3N_REG_ECC_GEOM0               0x0088
#define Q3N_REG_ECC_GEOM1               0x008c
#define Q3N_REG_ECC_STATUS              0x0090
#define Q3N_REG_ECC_MAX_BITFLIPS        0x0094
#define Q3N_REG_ECC_CORRECTED_BITS      0x0098
#define Q3N_REG_ECC_FAILED_STEP         0x009c
#define Q3N_REG_FAULT_STEP              0x00a0
#define Q3N_REG_FAULT_FIRST_BIT         0x00a4
#define Q3N_REG_FAULT_COUNT             0x00a8
#define Q3N_REG_FAULT_REGION            0x00ac
#define Q3N_REG_STAT_LDPC_CORRECTED     0x00b0
#define Q3N_REG_STAT_LDPC_UNCORRECTABLE 0x00b4
#define Q3N_REG_STAT_LDPC_FAILED_STEPS  0x00b8
#define Q3N_REG_MP_DIE                  0x00bc
#define Q3N_REG_MP_PLANE_MASK           0x00c0
#define Q3N_REG_MP_SLOT                 0x00c4
#define Q3N_REG_MP_ADDR_LO              0x00c8
#define Q3N_REG_MP_ADDR_HI              0x00cc
#define Q3N_REG_MP_SUCCESS_MASK         0x00d0
#define Q3N_REG_MP_FAILURE_MASK         0x00d4
#define Q3N_REG_MP_ECC_STATUS           0x00d8
#define Q3N_REG_MP_ECC_MAX_BITFLIPS     0x00dc
#define Q3N_REG_MP_ECC_CORRECTED_BITS   0x00e0
#define Q3N_REG_MP_ECC_FAILED_STEP      0x00e4
#define Q3N_REG_STAT_MP_COMMANDS        0x00e8
#define Q3N_REG_STAT_MP_SLOT_FAILURES   0x00ec
#define Q3N_REG_DATA                   0x1000

#define Q3N_CAP_BASIC_FLASH            BIT(0)
#define Q3N_CAP_SCHEME_D               Q3N_CAP_BASIC_FLASH
#define Q3N_CAP_PERSISTENT_MEDIA       BIT(1)
#define Q3N_CAP_BAD_BLOCK_MARKER       BIT(2)
#define Q3N_CAP_MULTIPLANE             BIT(3)

#define Q3N_MP_ALL_PLANES              0x0fU

#define Q3N_BLOCK_STATUS_BAD           BIT(0)

#define Q3N_STATUS_READY               BIT(0)
#define Q3N_STATUS_ERROR               BIT(1)
#define Q3N_STATUS_ECC_UNCORRECTABLE   BIT(2)

#define Q3N_ECC_STATUS_CLEAN           0U
#define Q3N_ECC_STATUS_CORRECTED       BIT(0)
#define Q3N_ECC_STATUS_UNCORRECTABLE   BIT(1)
#define Q3N_ECC_NO_FAILED_STEP         0xffffffffU

#define Q3N_FAULT_REGION_MAIN          0U
#define Q3N_FAULT_REGION_LDPC          1U

#define Q3N_CMD_READ_PAGE              2
#define Q3N_CMD_PROGRAM_PAGE           3
#define Q3N_CMD_ERASE_BLOCK            4
#define Q3N_CMD_RESET                  5
#define Q3N_CMD_READ_PAGE_OOB          6
#define Q3N_CMD_PROGRAM_PAGE_OOB       7
#define Q3N_CMD_GET_BLOCK_STATUS       8
#define Q3N_CMD_MP_READ_PAGE           9
#define Q3N_CMD_MP_PROGRAM_PAGE        10
#define Q3N_CMD_MP_READ_PAGE_OOB       11
#define Q3N_CMD_MP_PROGRAM_PAGE_OOB    12
#define Q3N_CMD_MP_ERASE_BLOCK         13

#define Q3N_FAULT_INJECT_DATA_LOSS     BIT(0)
#define Q3N_FAULT_FAIL_NEXT_PROGRAM    BIT(1)
#define Q3N_FAULT_INJECT_BITFLIPS      BIT(2)

#define Q3N_OP_FOREGROUND              0
#define Q3N_OP_PARITY_READ             1
#define Q3N_OP_PARITY_WRITE            2

#endif
