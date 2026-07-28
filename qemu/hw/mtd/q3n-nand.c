/*
 * QEMU 3D NAND base flash/controller model.
 *
 * This is a functional base model, not a timing-accurate NVDDR3 model. It
 * exposes physical block/page read, program, erase, geometry registers, and
 * data-loss fault injection. RAID layout, parity placement, and recovery are
 * intentionally left to the Linux driver.
 */

#include "qemu/osdep.h"
#include "hw/mtd/q3n-media.h"
#include "hw/mtd/q3n-nand.h"
#include "q3n-media-overlay.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/crc32c.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"

#define Q3N_ID_VALUE                  0x314e3351U /* "Q3N1" */

/* Q3N_CONTROLLER_HELPERS_BEGIN */
#define Q3N_LDPC_PROFILE_VERSION      1U
typedef struct Q3NEccResult {
    uint32_t status;
    uint32_t ecc_max_bitflips;
    uint32_t ecc_corrected_bits;
    uint32_t failed_step;
    uint32_t failed_steps;
} Q3NEccResult;

static bool q3n_retry_mode_valid(uint32_t mode)
{
    return mode < Q3N_READ_RETRY_MODES;
}

static uint32_t q3n_retry_gain(uint32_t mode)
{
    static const uint8_t gains[Q3N_READ_RETRY_MODES] = { 0, 8, 16, 24 };

    return q3n_retry_mode_valid(mode) ? gains[mode] : 0;
}

static uint32_t q3n_effective_bitflips(uint32_t raw_bitflips, uint32_t mode)
{
    uint32_t gain = q3n_retry_gain(mode);

    return raw_bitflips > gain ? raw_bitflips - gain : 0;
}

static void q3n_generate_ldpc_step(uint64_t page_key, uint32_t step,
                                   const uint8_t *data, uint8_t *ldpc)
{
    uint64_t page_key_le = cpu_to_le64(page_key);
    uint32_t step_le = cpu_to_le32(step);
    uint32_t profile_le = cpu_to_le32(Q3N_LDPC_PROFILE_VERSION);
    uint32_t crc;

    crc = crc32c(UINT32_MAX, (const uint8_t *)&page_key_le,
                 sizeof(page_key_le));
    crc = crc32c(crc ^ UINT32_MAX, (const uint8_t *)&step_le,
                 sizeof(step_le));
    crc = crc32c(crc ^ UINT32_MAX, (const uint8_t *)&profile_le,
                 sizeof(profile_le));
    crc = crc32c(crc ^ UINT32_MAX, data, Q3N_ECC_STEP_SIZE);

    for (uint32_t word = 0;
         word < Q3N_LDPC_BYTES_PER_STEP / sizeof(uint32_t); word++) {
        uint32_t counter_le = cpu_to_le32(word);
        uint32_t output_le;

        crc = crc32c(crc ^ UINT32_MAX, (const uint8_t *)&counter_le,
                     sizeof(counter_le));
        output_le = cpu_to_le32(crc);
        memcpy(ldpc + word * sizeof(output_le), &output_le,
               sizeof(output_le));
    }
}

static uint32_t q3n_overlay_popcount(const uint8_t *overlay, uint32_t size)
{
    uint32_t count = 0;

    for (uint32_t i = 0; i < size; i++) {
        count += ctpop8(overlay[i]);
    }
    return count;
}

static Q3NEccResult q3n_decode_ldpc(uint64_t page_key, const uint8_t *data,
                                    const uint8_t *ldpc,
                                    const uint8_t *main_overlay,
                                    const uint8_t *ldpc_overlay, bool erased,
                                    uint32_t retry_mode)
{
    Q3NEccResult result = {
        .status = Q3N_ECC_STATUS_CLEAN,
        .failed_step = Q3N_ECC_NO_FAILED_STEP,
    };

    for (uint32_t step = 0; step < Q3N_LDPC_STEPS; step++) {
        const uint8_t *main_errors =
            main_overlay + step * Q3N_ECC_STEP_SIZE;
        const uint8_t *ldpc_errors =
            ldpc_overlay + step * Q3N_LDPC_BYTES_PER_STEP;
        const uint8_t *stored_ldpc =
            ldpc + step * Q3N_LDPC_BYTES_PER_STEP;
        uint8_t expected_ldpc[Q3N_LDPC_BYTES_PER_STEP];
        uint32_t bitflips;
        bool ldpc_matches = true;

        bitflips = q3n_overlay_popcount(main_errors, Q3N_ECC_STEP_SIZE) +
                   q3n_overlay_popcount(ldpc_errors,
                                        Q3N_LDPC_BYTES_PER_STEP);
        bitflips = q3n_effective_bitflips(bitflips, retry_mode);
        if (!erased) {
            q3n_generate_ldpc_step(page_key, step,
                                   data + step * Q3N_ECC_STEP_SIZE,
                                   expected_ldpc);
            ldpc_matches = !memcmp(stored_ldpc, expected_ldpc,
                                   sizeof(expected_ldpc));
        }
        if (!ldpc_matches || bitflips > Q3N_ECC_STRENGTH) {
            if (result.failed_step == Q3N_ECC_NO_FAILED_STEP) {
                result.failed_step = step;
            }
            result.failed_steps++;
            continue;
        }
        if (bitflips) {
            result.ecc_corrected_bits += bitflips;
            if (bitflips > result.ecc_max_bitflips) {
                result.ecc_max_bitflips = bitflips;
            }
        }
    }

    if (result.failed_steps) {
        result.status = Q3N_ECC_STATUS_UNCORRECTABLE;
    } else if (result.ecc_corrected_bits) {
        result.status = Q3N_ECC_STATUS_CORRECTED;
    }
    return result;
}

static bool q3n_oob_transfer_valid(uint32_t data_count, uint32_t oob_len,
                                   bool program)
{
    return oob_len == Q3N_LOGICAL_OOB_SIZE &&
           (!program || data_count >= Q3N_LOGICAL_OOB_SIZE);
}

static void q3n_reset_oob_staging(uint32_t *data_pos, uint32_t *data_count)
{
    *data_pos = 0;
    *data_count = 0;
}
/* Q3N_CONTROLLER_HELPERS_END */

typedef struct Q3NStats {
    uint64_t page_programs;
    uint64_t block_erases;
    uint64_t page_read_errors;
    uint64_t faults_injected;
    uint64_t fg_ops;
    uint64_t parity_reads;
    uint64_t parity_writes;
    uint64_t ldpc_corrected_bits;
    uint64_t ldpc_uncorrectable_pages;
    uint64_t ldpc_failed_steps;
} Q3NStats;

struct Q3NNandState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t data_blocks_per_plane;
    uint32_t parity_blocks_per_plane;
    uint32_t metadata_blocks_per_plane;
    uint32_t reserve_blocks_per_plane;

    uint32_t status;
    uint32_t irq_status;
    uint32_t irq_mask;
    uint32_t cmd;
    uint32_t len;
    uint32_t oob_len;
    uint64_t addr;
    uint64_t fault_addr;
    uint32_t fault_ctrl;
    uint32_t fault_step;
    uint32_t fault_first_bit;
    uint32_t fault_count;
    uint32_t fault_region;
    uint32_t op_class;
    uint32_t read_flags;
    uint32_t retry_mode;
    uint64_t fail_program_addr;
    bool fail_next_program;
    uint32_t block_status;
    uint32_t ecc_status;
    uint32_t ecc_max_bitflips;
    uint32_t ecc_corrected_bits;
    uint32_t ecc_failed_step;

    uint8_t data_buf[Q3N_PAGE_SIZE + Q3N_LOGICAL_OOB_SIZE];
    uint8_t ldpc[Q3N_PHYSICAL_LDPC_SIZE];
    uint8_t main_overlay[Q3N_PAGE_SIZE];
    uint8_t ldpc_overlay[Q3N_LDPC_TOTAL_BYTES];
    uint32_t data_pos;
    uint32_t data_count;

    uint32_t physical_block_count;
    uint64_t physical_size;
    uint64_t erase_size;

    BlockBackend *blk;
    Q3NMedia *media;

    Q3NStats stats;
};

static bool q3n_decode_addr(Q3NNandState *s, uint64_t byte_addr,
                            uint32_t *block, uint32_t *page,
                            uint32_t *column)
{
    uint64_t physical_page = byte_addr / Q3N_PAGE_SIZE;

    *column = byte_addr % Q3N_PAGE_SIZE;
    *block = physical_page / Q3N_PAGES_PER_BLOCK;
    *page = physical_page % Q3N_PAGES_PER_BLOCK;

    return *block < s->physical_block_count;
}

static int q3n_read_page(Q3NNandState *s, uint32_t block, uint32_t page,
                         uint8_t *buf, bool raw)
{
    uint64_t page_key = (uint64_t)block * Q3N_PAGES_PER_BLOCK + page;
    bool erased;
    Q3NEccResult result;
    int ret;

    ret = q3n_media_read_page(s->media, block, page, buf, s->ldpc,
                              s->main_overlay, s->ldpc_overlay);

    if (ret) {
        s->stats.page_read_errors++;
        return ret;
    }

    if (raw) {
        for (uint32_t i = 0; i < Q3N_PAGE_SIZE; i++) {
            buf[i] ^= s->main_overlay[i];
        }
        return 0;
    }

    erased = q3n_media_is_erased(buf, Q3N_PAGE_SIZE) &&
             q3n_media_is_erased(s->ldpc, Q3N_PHYSICAL_LDPC_SIZE);

    result = q3n_decode_ldpc(page_key, buf, s->ldpc,
                             s->main_overlay, s->ldpc_overlay, erased,
                             s->retry_mode);
    s->ecc_status = result.status;
    s->ecc_max_bitflips = result.ecc_max_bitflips;
    s->ecc_corrected_bits = result.ecc_corrected_bits;
    s->ecc_failed_step = result.failed_step;
    s->stats.ldpc_corrected_bits += result.ecc_corrected_bits;
    if (result.status == Q3N_ECC_STATUS_UNCORRECTABLE) {
        s->status |= Q3N_STATUS_ECC_UNCORRECTABLE;
        s->stats.ldpc_uncorrectable_pages++;
        s->stats.ldpc_failed_steps += result.failed_steps;
        for (uint32_t i = 0; i < Q3N_PAGE_SIZE; i++) {
            buf[i] ^= s->main_overlay[i];
        }
    }
    return 0;
}

static void q3n_disarm_program_fault(Q3NNandState *s)
{
    s->fail_next_program = false;
    s->fail_program_addr = 0;
    s->fault_ctrl &= ~Q3N_FAULT_FAIL_NEXT_PROGRAM;
}

static bool q3n_program_page(Q3NNandState *s, uint32_t block,
                             uint32_t page, const uint8_t *buf)
{
    uint8_t ldpc[Q3N_PHYSICAL_LDPC_SIZE];
    uint64_t page_key = (uint64_t)block * Q3N_PAGES_PER_BLOCK + page;
    int ret;

    if (block >= s->physical_block_count || page >= Q3N_PAGES_PER_BLOCK) {
        return false;
    }
    if (s->fail_next_program && s->addr == s->fail_program_addr) {
        q3n_disarm_program_fault(s);
        s->stats.faults_injected++;
        return false;
    }

    for (uint32_t step = 0; step < Q3N_LDPC_STEPS; step++) {
        q3n_generate_ldpc_step(page_key, step,
                               buf + step * Q3N_ECC_STEP_SIZE,
                               ldpc + step * Q3N_LDPC_BYTES_PER_STEP);
    }

    ret = q3n_media_program_page(s->media, block, page, buf, ldpc);
    if (ret) {
        return false;
    }
    s->stats.page_programs++;
    return true;
}

static bool q3n_inject_data_loss(Q3NNandState *s, uint64_t byte_addr)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;
    if (!q3n_decode_addr(s, byte_addr, &block, &page, &column) || column != 0 ||
        q3n_media_inject_bitflips(s->media, block, page, 0,
                                  Q3N_FAULT_REGION_MAIN, 0,
                                  Q3N_ECC_STRENGTH + 1)) {
        return false;
    }

    s->stats.faults_injected++;
    return true;
}

static bool q3n_inject_bitflips(Q3NNandState *s, uint64_t byte_addr)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;

    if (!q3n_decode_addr(s, byte_addr, &block, &page, &column) || column != 0 ||
        q3n_media_inject_bitflips(s->media, block, page, s->fault_step,
                                 s->fault_region, s->fault_first_bit,
                                 s->fault_count)) {
        return false;
    }

    s->stats.faults_injected++;
    return true;
}

static bool q3n_erase_block(Q3NNandState *s, uint32_t block)
{
    if (block >= s->physical_block_count ||
        q3n_media_erase_block(s->media, block)) {
        return false;
    }
    s->stats.block_erases++;
    return true;
}

static void q3n_raise_irq(Q3NNandState *s, uint32_t bits)
{
    s->irq_status |= bits;
    if (s->irq_status & s->irq_mask) {
        qemu_irq_raise(s->irq);
    }
}

static void q3n_clear_error(Q3NNandState *s)
{
    s->status &= ~Q3N_STATUS_ERROR;
}

static void q3n_clear_ecc_result(Q3NNandState *s)
{
    s->status &= ~Q3N_STATUS_ECC_UNCORRECTABLE;
    s->ecc_status = Q3N_ECC_STATUS_CLEAN;
    s->ecc_max_bitflips = 0;
    s->ecc_corrected_bits = 0;
    s->ecc_failed_step = Q3N_ECC_NO_FAILED_STEP;
}

static void q3n_finish_ok(Q3NNandState *s)
{
    s->status |= Q3N_STATUS_READY;
    q3n_raise_irq(s, Q3N_IRQ_DONE);
}

static void q3n_finish_error(Q3NNandState *s)
{
    s->status |= Q3N_STATUS_READY | Q3N_STATUS_ERROR;
    q3n_raise_irq(s, Q3N_IRQ_DONE | Q3N_IRQ_ERROR);
}

static void q3n_cmd_read_id(Q3NNandState *s)
{
    static const uint8_t id[] = {
        0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44
    };

    memset(s->data_buf, 0xff, sizeof(s->data_buf));
    memcpy(s->data_buf, id, sizeof(id));
    s->data_count = sizeof(id);
    s->data_pos = 0;
    q3n_finish_ok(s);
}

static void q3n_cmd_read_page(Q3NNandState *s)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;
    bool raw = s->read_flags & Q3N_READ_F_RAW;

    if (!raw) {
        q3n_clear_ecc_result(s);
    }
    if (!q3n_decode_addr(s, s->addr, &block, &page, &column) || column != 0) {
        q3n_finish_error(s);
        return;
    }

    if (s->op_class == Q3N_OP_PARITY_READ) {
        s->stats.parity_reads++;
    } else {
        s->stats.fg_ops++;
    }

    if (q3n_read_page(s, block, page, s->data_buf, raw)) {
        q3n_finish_error(s);
        return;
    }

    s->data_count = Q3N_PAGE_SIZE;
    s->data_pos = 0;
    q3n_finish_ok(s);
}

static void q3n_cmd_program_page(Q3NNandState *s)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;

    if (s->data_count < Q3N_PAGE_SIZE ||
        !q3n_decode_addr(s, s->addr, &block, &page, &column) ||
        column != 0) {
        q3n_finish_error(s);
        return;
    }

    if (s->op_class == Q3N_OP_PARITY_WRITE) {
        s->stats.parity_writes++;
    } else {
        s->stats.fg_ops++;
    }
    if (!q3n_program_page(s, block, page, s->data_buf)) {
        q3n_finish_error(s);
        return;
    }

    q3n_finish_ok(s);
}

static void q3n_cmd_read_page_oob(Q3NNandState *s)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;

    if (!q3n_decode_addr(s, s->addr, &block, &page, &column) || column != 0 ||
        !q3n_oob_transfer_valid(s->data_count, s->oob_len, false) ||
        q3n_media_read_logical_oob(s->media, block, page, s->data_buf)) {
        q3n_finish_error(s);
        return;
    }

    if (s->op_class == Q3N_OP_PARITY_READ) {
        s->stats.parity_reads++;
    } else {
        s->stats.fg_ops++;
    }

    s->data_count = Q3N_LOGICAL_OOB_SIZE;
    s->data_pos = 0;
    q3n_finish_ok(s);
}

static void q3n_cmd_program_page_oob(Q3NNandState *s)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;

    if (!q3n_oob_transfer_valid(s->data_count, s->oob_len, true) ||
        !q3n_decode_addr(s, s->addr, &block, &page, &column) || column != 0) {
        q3n_finish_error(s);
        return;
    }

    if (q3n_media_program_logical_oob(s->media, block, page,
                                      s->data_buf)) {
        q3n_finish_error(s);
        return;
    }

    s->stats.page_programs++;
    if (s->op_class == Q3N_OP_PARITY_WRITE) {
        s->stats.parity_writes++;
    } else {
        s->stats.fg_ops++;
    }

    q3n_finish_ok(s);
}

static void q3n_cmd_erase_block(Q3NNandState *s)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;

    if (!q3n_decode_addr(s, s->addr, &block, &page, &column) ||
        page != 0 || column != 0 || !q3n_erase_block(s, block)) {
        q3n_finish_error(s);
        return;
    }

    q3n_finish_ok(s);
}

static bool q3n_decode_block_addr(Q3NNandState *s, uint32_t *block)
{
    if (s->addr >= s->physical_size || s->addr % s->erase_size) {
        return false;
    }
    *block = s->addr / s->erase_size;
    return true;
}

static void q3n_cmd_get_block_status(Q3NNandState *s)
{
    uint32_t block;

    if (!q3n_decode_block_addr(s, &block) ||
        q3n_media_get_block_status(s->media, block, &s->block_status)) {
        q3n_finish_error(s);
        return;
    }
    q3n_finish_ok(s);
}

static void q3n_cmd_reset(Q3NNandState *s)
{
    q3n_disarm_program_fault(s);
    q3n_clear_error(s);
    s->cmd = Q3N_CMD_NOP;
    s->data_pos = 0;
    s->data_count = 0;
    s->read_flags = 0;
    s->retry_mode = 0;
    s->irq_status = 0;
    qemu_irq_lower(s->irq);
    q3n_clear_ecc_result(s);
    q3n_finish_ok(s);
}

static void q3n_execute_cmd(Q3NNandState *s, uint32_t cmd)
{
    s->cmd = cmd;
    s->status &= ~Q3N_STATUS_READY;
    q3n_clear_error(s);

    switch (cmd) {
    case Q3N_CMD_NOP:
        q3n_finish_ok(s);
        break;
    case Q3N_CMD_READ_ID:
        q3n_cmd_read_id(s);
        break;
    case Q3N_CMD_READ_PAGE:
        q3n_cmd_read_page(s);
        break;
    case Q3N_CMD_PROGRAM_PAGE:
        q3n_cmd_program_page(s);
        break;
    case Q3N_CMD_READ_PAGE_OOB:
        q3n_cmd_read_page_oob(s);
        break;
    case Q3N_CMD_PROGRAM_PAGE_OOB:
        q3n_cmd_program_page_oob(s);
        break;
    case Q3N_CMD_ERASE_BLOCK:
        q3n_cmd_erase_block(s);
        break;
    case Q3N_CMD_GET_BLOCK_STATUS:
        q3n_cmd_get_block_status(s);
        break;
    case Q3N_CMD_RESET:
        q3n_cmd_reset(s);
        break;
    default:
        q3n_finish_error(s);
        break;
    }
}

static uint32_t q3n_read_data_window(Q3NNandState *s, unsigned size)
{
    uint32_t value = 0xffffffffU;
    unsigned i;

    if (s->data_pos >= s->data_count) {
        return value;
    }

    value = 0;
    for (i = 0; i < size; i++) {
        uint32_t shift = i * 8;
        uint8_t byte = 0xff;

        if (s->data_pos < s->data_count) {
            byte = s->data_buf[s->data_pos++];
        }
        value |= (uint32_t)byte << shift;
    }

    return value;
}

static void q3n_write_data_window(Q3NNandState *s, uint64_t value, unsigned size)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        if (s->data_pos >= sizeof(s->data_buf)) {
            s->status |= Q3N_STATUS_ERROR;
            return;
        }
        s->data_buf[s->data_pos++] = extract32(value, i * 8, 8);
    }

    if (s->data_count < s->data_pos) {
        s->data_count = s->data_pos;
    }
}

static uint64_t q3n_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    Q3NNandState *s = opaque;

    if (offset >= Q3N_REG_DATA &&
        offset < Q3N_REG_DATA + Q3N_PAGE_SIZE + Q3N_LOGICAL_OOB_SIZE) {
        return q3n_read_data_window(s, size);
    }

    switch (offset) {
    case Q3N_REG_ID:
        return Q3N_ID_VALUE;
    case Q3N_REG_CAP:
        return Q3N_CAP_BASIC_FLASH | Q3N_CAP_PERSISTENT_MEDIA |
               Q3N_CAP_BAD_BLOCK_MARKER | Q3N_CAP_READ_RETRY;
    case Q3N_REG_STATUS:
        return s->status;
    case Q3N_REG_CMD:
        return s->cmd;
    case Q3N_REG_ADDR_LO:
        return (uint32_t)s->addr;
    case Q3N_REG_ADDR_HI:
        return (uint32_t)(s->addr >> 32);
    case Q3N_REG_LEN:
        return s->len;
    case Q3N_REG_OOB_LEN:
        return s->oob_len;
    case Q3N_REG_OP_CLASS:
        return s->op_class;
    case Q3N_REG_GEOM0:
        return (Q3N_PAGE_SIZE & 0xffffU) |
               (Q3N_LOGICAL_OOB_SIZE << 16);
    case Q3N_REG_GEOM1:
        return (Q3N_PAGES_PER_BLOCK & 0xffffU) |
               (Q3N_BLOCKS_PER_PLANE << 16);
    case Q3N_REG_POOL0:
        return (s->data_blocks_per_plane & 0xffffU) |
               (s->parity_blocks_per_plane << 16);
    case Q3N_REG_POOL1:
        return (s->metadata_blocks_per_plane & 0xffffU) |
               (s->reserve_blocks_per_plane << 16);
    case Q3N_REG_IRQ_STATUS:
        return s->irq_status;
    case Q3N_REG_IRQ_MASK:
        return s->irq_mask;
    case Q3N_REG_STAT_PAGE_PROGRAMS:
        return (uint32_t)s->stats.page_programs;
    case Q3N_REG_STAT_BLOCK_ERASES:
        return (uint32_t)s->stats.block_erases;
    case Q3N_REG_STAT_PAGE_READ_ERRORS:
        return (uint32_t)s->stats.page_read_errors;
    case Q3N_REG_STAT_FAULTS_INJECTED:
        return (uint32_t)s->stats.faults_injected;
    case Q3N_REG_STAT_FG_OPS:
        return (uint32_t)s->stats.fg_ops;
    case Q3N_REG_STAT_PARITY_READS:
        return (uint32_t)s->stats.parity_reads;
    case Q3N_REG_STAT_PARITY_WRITES:
        return (uint32_t)s->stats.parity_writes;
    case Q3N_REG_BLOCK_STATUS:
        return s->block_status;
    case Q3N_REG_FAULT_ADDR_LO:
        return (uint32_t)s->fault_addr;
    case Q3N_REG_FAULT_ADDR_HI:
        return (uint32_t)(s->fault_addr >> 32);
    case Q3N_REG_FAULT_CTRL:
        return s->fault_ctrl;
    case Q3N_REG_ECC_GEOM0:
        return (Q3N_ECC_STEP_SIZE & 0xffffU) |
               (Q3N_ECC_STRENGTH << 16);
    case Q3N_REG_ECC_GEOM1:
        return (Q3N_LDPC_BYTES_PER_STEP & 0xffffU) |
               (Q3N_LDPC_STEPS << 16);
    case Q3N_REG_ECC_STATUS:
        return s->ecc_status;
    case Q3N_REG_ECC_MAX_BITFLIPS:
        return s->ecc_max_bitflips;
    case Q3N_REG_ECC_CORRECTED_BITS:
        return s->ecc_corrected_bits;
    case Q3N_REG_ECC_FAILED_STEP:
        return s->ecc_failed_step;
    case Q3N_REG_FAULT_STEP:
        return s->fault_step;
    case Q3N_REG_FAULT_FIRST_BIT:
        return s->fault_first_bit;
    case Q3N_REG_FAULT_COUNT:
        return s->fault_count;
    case Q3N_REG_FAULT_REGION:
        return s->fault_region;
    case Q3N_REG_STAT_LDPC_CORRECTED:
        return (uint32_t)s->stats.ldpc_corrected_bits;
    case Q3N_REG_STAT_LDPC_UNCORRECTABLE:
        return (uint32_t)s->stats.ldpc_uncorrectable_pages;
    case Q3N_REG_STAT_LDPC_FAILED_STEPS:
        return (uint32_t)s->stats.ldpc_failed_steps;
    case Q3N_REG_READ_FLAGS:
        return s->read_flags;
    case Q3N_REG_RETRY_MODE:
        return s->retry_mode;
    default:
        return 0;
    }
}

static void q3n_mmio_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    Q3NNandState *s = opaque;

    if (offset >= Q3N_REG_DATA &&
        offset < Q3N_REG_DATA + Q3N_PAGE_SIZE + Q3N_LOGICAL_OOB_SIZE) {
        q3n_write_data_window(s, value, size);
        return;
    }

    switch (offset) {
    case Q3N_REG_CTRL:
        if (value & 1U) {
            q3n_execute_cmd(s, Q3N_CMD_RESET);
        }
        break;
    case Q3N_REG_CMD:
        q3n_execute_cmd(s, value);
        break;
    case Q3N_REG_ADDR_LO:
        s->addr = (s->addr & 0xffffffff00000000ULL) | (uint32_t)value;
        break;
    case Q3N_REG_ADDR_HI:
        s->addr = ((uint64_t)(uint32_t)value << 32) | (uint32_t)s->addr;
        break;
    case Q3N_REG_LEN:
        s->len = value;
        s->data_pos = 0;
        s->data_count = 0;
        break;
    case Q3N_REG_OOB_LEN:
        s->oob_len = value;
        q3n_reset_oob_staging(&s->data_pos, &s->data_count);
        break;
    case Q3N_REG_OP_CLASS:
        if (value <= Q3N_OP_PARITY_WRITE) {
            s->op_class = value;
        }
        break;
    case Q3N_REG_READ_FLAGS:
        if (!(value & ~Q3N_READ_F_RAW)) {
            s->read_flags = value;
        }
        break;
    case Q3N_REG_RETRY_MODE:
        if (q3n_retry_mode_valid(value)) {
            s->retry_mode = value;
        } else {
            s->status |= Q3N_STATUS_ERROR;
        }
        break;
    case Q3N_REG_FAULT_ADDR_LO:
        s->fault_addr = (s->fault_addr & 0xffffffff00000000ULL) |
                        (uint32_t)value;
        break;
    case Q3N_REG_FAULT_ADDR_HI:
        s->fault_addr = ((uint64_t)(uint32_t)value << 32) |
                        (uint32_t)s->fault_addr;
        break;
    case Q3N_REG_FAULT_STEP:
        s->fault_step = value;
        break;
    case Q3N_REG_FAULT_FIRST_BIT:
        s->fault_first_bit = value;
        break;
    case Q3N_REG_FAULT_COUNT:
        s->fault_count = value;
        break;
    case Q3N_REG_FAULT_REGION:
        s->fault_region = value;
        break;
    case Q3N_REG_FAULT_CTRL:
        q3n_disarm_program_fault(s);
        s->fault_ctrl = 0;
        q3n_clear_error(s);
        if ((value & Q3N_FAULT_INJECT_DATA_LOSS) &&
            !q3n_inject_data_loss(s, s->fault_addr)) {
            s->status |= Q3N_STATUS_ERROR;
        }
        if ((value & Q3N_FAULT_INJECT_BITFLIPS) &&
            !q3n_inject_bitflips(s, s->fault_addr)) {
            s->status |= Q3N_STATUS_ERROR;
        }
        if (value & Q3N_FAULT_FAIL_NEXT_PROGRAM) {
            uint32_t block;
            uint32_t page;
            uint32_t column;

            if (!q3n_decode_addr(s, s->fault_addr, &block, &page,
                                 &column) || column != 0) {
                s->status |= Q3N_STATUS_ERROR;
            } else {
                s->fail_program_addr = s->fault_addr;
                s->fail_next_program = true;
                s->fault_ctrl |= Q3N_FAULT_FAIL_NEXT_PROGRAM;
            }
        }
        break;
    case Q3N_REG_IRQ_STATUS:
        s->irq_status &= ~(uint32_t)value;
        if (!(s->irq_status & s->irq_mask)) {
            qemu_irq_lower(s->irq);
        }
        break;
    case Q3N_REG_IRQ_MASK:
        s->irq_mask = value;
        if (s->irq_status & s->irq_mask) {
            qemu_irq_raise(s->irq);
        } else {
            qemu_irq_lower(s->irq);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps q3n_mmio_ops = {
    .read = q3n_mmio_read,
    .write = q3n_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

MemoryRegion *q3n_nand_get_mmio(Q3NNandState *s)
{
    return &s->mmio;
}

void q3n_nand_set_irq(Q3NNandState *s, qemu_irq irq)
{
    s->irq = irq;
}

static void q3n_realize(DeviceState *dev, Error **errp)
{
    Q3NNandState *s = Q3N_NAND(dev);
    uint32_t pool_sum = s->data_blocks_per_plane +
                        s->parity_blocks_per_plane +
                        s->metadata_blocks_per_plane +
                        s->reserve_blocks_per_plane;
    if (pool_sum > Q3N_BLOCKS_PER_PLANE) {
        error_setg(errp, "q3n-nand block pools exceed %u blocks per plane",
                   Q3N_BLOCKS_PER_PLANE);
        return;
    }

    s->physical_block_count = Q3N_BLOCKS_PER_PLANE * Q3N_LANES;
    s->erase_size = (uint64_t)Q3N_PAGES_PER_BLOCK * Q3N_PAGE_SIZE;
    s->physical_size = (uint64_t)s->physical_block_count * s->erase_size;
    s->media = q3n_media_open(s->blk, s->physical_block_count,
                              Q3N_PAGES_PER_BLOCK, Q3N_PAGE_SIZE,
                              Q3N_OOB_SIZE, errp);
    if (!s->media) {
        return;
    }
    s->status = Q3N_STATUS_READY;
    q3n_clear_ecc_result(s);
}

static void q3n_unrealize(DeviceState *dev)
{
    Q3NNandState *s = Q3N_NAND(dev);

    q3n_media_close(s->media);
    s->media = NULL;
}

static void q3n_instance_init(Object *obj)
{
    Q3NNandState *s = Q3N_NAND(obj);

    memory_region_init_io(&s->mmio, obj, &q3n_mmio_ops, s, TYPE_Q3N_NAND,
                          Q3N_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property q3n_properties[] = {
    DEFINE_PROP_DRIVE("drive", Q3NNandState, blk),
    DEFINE_PROP_UINT32("data-blocks-per-plane", Q3NNandState,
                       data_blocks_per_plane,
                       Q3N_DEFAULT_DATA_BLOCKS_PER_PLANE),
    DEFINE_PROP_UINT32("parity-blocks-per-plane", Q3NNandState,
                       parity_blocks_per_plane,
                       Q3N_DEFAULT_PARITY_BLOCKS_PER_PLANE),
    DEFINE_PROP_UINT32("metadata-blocks-per-plane", Q3NNandState,
                       metadata_blocks_per_plane,
                       Q3N_DEFAULT_METADATA_BLOCKS_PER_PLANE),
    DEFINE_PROP_UINT32("reserve-blocks-per-plane", Q3NNandState,
                       reserve_blocks_per_plane,
                       Q3N_DEFAULT_RESERVE_BLOCKS_PER_PLANE),
};

static void q3n_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = q3n_realize;
    dc->unrealize = q3n_unrealize;
    device_class_set_props(dc, q3n_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo q3n_type_info = {
    .name = TYPE_Q3N_NAND,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Q3NNandState),
    .instance_init = q3n_instance_init,
    .class_init = q3n_class_init,
};

static void q3n_register_types(void)
{
    type_register_static(&q3n_type_info);
}

type_init(q3n_register_types)
